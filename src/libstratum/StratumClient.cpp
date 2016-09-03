#include "StratumClient.h"
#include "util.h"

#include "json/json_spirit_reader_template.h"
#include "json/json_spirit_utils.h"

using boost::asio::ip::tcp;
using namespace json_spirit;

#define LogS(...) LogPrint("stratum", __VA_ARGS__)


static void diffToTarget(uint32_t *target, double diff)
{
    uint32_t target2[8];
    uint64_t m;
    int k;

    for (k = 6; k > 0 && diff > 1.0; k--)
        diff /= 4294967296.0;
    m = (uint64_t)(4294901760.0 / diff);
    if (m == 0 && k == 6)
        memset(target2, 0xff, 32);
    else {
        memset(target2, 0, 32);
        target2[k] = (uint32_t)m;
        target2[k + 1] = (uint32_t)(m >> 32);
    }

    for (int i = 0; i < 32; i++)
        ((uint8_t*)target)[31 - i] = ((uint8_t*)target2)[i];
}


template <typename Miner, typename Job, typename Solution>
StratumClient<Miner, Job, Solution>::StratumClient(
        Miner * m,
        string const & host, string const & port,
        string const & user, string const & pass,
        int const & retries, int const & worktimeout)
    : m_socket(m_io_service)
{
    m_primary.host = host;
    m_primary.port = port;
    m_primary.user = user;
    m_primary.pass = pass;

    p_active = &m_primary;

    m_authorized = false;
    m_connected = false;
    m_maxRetries = retries;
    m_worktimeout = worktimeout;

    p_miner = m;
    p_current = nullptr;
    p_previous = nullptr;
    p_worktimer = nullptr;
    startWorking();
}

template <typename Miner, typename Job, typename Solution>
void StratumClient<Miner, Job, Solution>::setFailover(
        string const & host, string const & port)
{
    setFailover(host, port, p_active->user, p_active->pass);
}

template <typename Miner, typename Job, typename Solution>
void StratumClient<Miner, Job, Solution>::setFailover(
        string const & host, string const & port,
        string const & user, string const & pass)
{
    m_failover.host = host;
    m_failover.port = port;
    m_failover.user = user;
    m_failover.pass = pass;
}

template <typename Miner, typename Job, typename Solution>
void StratumClient<Miner, Job, Solution>::startWorking()
{
    m_work.reset(new std::thread([&]() {
        workLoop();
    }));
}

template <typename Miner, typename Job, typename Solution>
void StratumClient<Miner, Job, Solution>::workLoop()
{
    while (m_running) {
        try {
            if (!m_connected) {
                //m_io_service.run();
                //boost::thread t(boost::bind(&boost::asio::io_service::run, &m_io_service));
                connect();

            }
            read_until(m_socket, m_responseBuffer, "\n");
            std::istream is(&m_responseBuffer);
            std::string response;
            getline(is, response);

            if (!response.empty() && response.front() == '{' && response.back() == '}') {
                Value valResponse;
                if (read_string(response, valResponse) && valResponse.type() == obj_type) {
                    const Object& responseObject = valResponse.get_obj();
                    if (!responseObject.empty()) {
                        processReponse(responseObject);
                        m_response = response;
                    } else {
                        LogS("[WARN] Response was empty\n");
                    }
                } else {
                    LogS("[WARN] Parse response failed\n");
                }
            } else {
                LogS("[WARN] Discarding incomplete response\n");
            }
        } catch (std::exception const& _e) {
            LogS("[WARN] %s\n", _e.what());
            reconnect();
        }
    }
}


template <typename Miner, typename Job, typename Solution>
void StratumClient<Miner, Job, Solution>::connect()
{
    LogS("Connecting to stratum server %s:%s\n", p_active->host, p_active->port);

    tcp::resolver r(m_io_service);
    tcp::resolver::query q(p_active->host, p_active->port);
    tcp::resolver::iterator endpoint_iterator = r.resolve(q);
    tcp::resolver::iterator end;

    boost::system::error_code error = boost::asio::error::host_not_found;
    while (error && endpoint_iterator != end) {
        m_socket.close();
        m_socket.connect(*endpoint_iterator++, error);
    }
    if (error) {
        LogS("[ERROR] Could not connect to stratum server %s:%s, %s\n",
             p_active->host, p_active->port, error.message());
        reconnect();
    } else {
        LogS("Connected!\n");
        m_connected = true;
        if (!p_miner->isMining()) {
            LogS("Starting miner\n");
            p_miner->start();
        }
        std::ostream os(&m_requestBuffer);
        os << "{\"id\": 1, \"method\": \"mining.subscribe\", \"params\": [\""
           << p_active->host << "\",\""
           << p_active->port << "\",\""
           << p_miner->userAgent() << "\", null]}\n";
        write(m_socket, m_requestBuffer);
    }
}

template <typename Miner, typename Job, typename Solution>
void StratumClient<Miner, Job, Solution>::reconnect()
{
    if (p_worktimer) {
        p_worktimer->cancel();
        p_worktimer = nullptr;
    }

    //m_io_service.reset();
    //m_socket.close(); // leads to crashes on Linux
    m_authorized = false;
    m_connected = false;

    if (!m_failover.host.empty()) {
        m_retries++;

        if (m_retries > m_maxRetries) {
            if (m_failover.host == "exit") {
                disconnect();
                return;
            } else if (p_active == &m_primary) {
                p_active = &m_failover;
            } else {
                p_active = &m_primary;
            }
            m_retries = 0;
        }
    }

    LogS("Reconnecting in 3 seconds...\n");
    boost::asio::deadline_timer timer(m_io_service, boost::posix_time::seconds(3));
    timer.wait();
}

template <typename Miner, typename Job, typename Solution>
void StratumClient<Miner, Job, Solution>::disconnect()
{
    if (!m_connected) return;
    LogS("Disconnecting\n");
    m_connected = false;
    m_running = false;
    if (p_miner->isMining()) {
        LogS("Stopping miner\n");
        p_miner->stop();
    }
    m_socket.close();
    //m_io_service.stop();
    if (m_work) {
        m_work->join();
        m_work.reset();
    }
}

template <typename Miner, typename Job, typename Solution>
void StratumClient<Miner, Job, Solution>::processReponse(const Object& responseObject)
{
    const Value& valError = find_value(responseObject, "error");
    if (valError.type() == array_type) {
        const Array& error = valError.get_array();
        string msg;
        if (error.size() > 0 && error[1].type() == str_type) {
            msg = error[1].get_str();
        } else {
            msg = "Unknown error";
        }
        LogS("%s\n", msg);
    }
    std::ostream os(&m_requestBuffer);
    const Value& valId = find_value(responseObject, "id");
    int id = 0;
    if (valId.type() == int_type) {
        id = valId.get_int();
    }
    Value valRes;
    bool accepted = false;
    switch (id) {
    case 1:
        valRes = find_value(responseObject, "result");
        if (valRes.type() == array_type) {
            LogS("Subscribed to stratum server\n");
            const Array& result = valRes.get_array();
            // Ignore session ID for now.
            p_miner->setServerNonce(result);
            os << "{\"id\": 2, \"method\": \"mining.authorize\", \"params\": [\""
               << p_active->user << "\",\"" << p_active->pass << "\"]}\n";
            write(m_socket, m_requestBuffer);
        }
        break;
    case 2:
        valRes = find_value(responseObject, "result");
        m_authorized = false;
        if (valRes.type() == bool_type) {
            m_authorized = valRes.get_bool();
        }
        if (!m_authorized) {
            LogS("Worker not authorized: %s\n", p_active->user);
            disconnect();
            return;
        }
        LogS("Authorized worker %s\n", p_active->user);
        break;
    case 3:
        // nothing to do...
        break;
    case 4:
        valRes = find_value(responseObject, "result");
        if (valRes.type() == bool_type) {
            accepted = valRes.get_bool();
        }
        if (accepted) {
            LogS("B-) Submitted and accepted.\n");
            p_miner->acceptedSolution(m_stale);
        } else {
            LogS("[WARN] :-( Not accepted.\n");
            p_miner->rejectedSolution(m_stale);
        }
        break;
    default:
        const Value& valMethod = find_value(responseObject, "method");
        string method = "";
        if (valMethod.type() == str_type) {
            method = valMethod.get_str();
        }

        if (method == "mining.notify") {
            const Value& valParams = find_value(responseObject, "params");
            if (valParams.type() == array_type) {
                const Array& params = valParams.get_array();
                Job* workOrder = p_miner->parseJob(params);

                if (workOrder) {
                    LogS("Received new job #%s\n", workOrder->jobId());
                    workOrder->setDifficulty(m_nextJobDifficulty);

                    if (!(p_current && *workOrder == *p_current)) {
                        //x_current.lock();
                        //if (p_worktimer)
                        //    p_worktimer->cancel();

                        if (p_previous) {
                            delete p_previous;
                        }
                        p_previous = p_current;
                        p_current = workOrder;

                        p_miner->setJob(p_current);
                        //x_current.unlock();
                        //p_worktimer = new boost::asio::deadline_timer(m_io_service, boost::posix_time::seconds(m_worktimeout));
                        //p_worktimer->async_wait(boost::bind(&StratumClient::work_timeout_handler, this, boost::asio::placeholders::error));
                    }
                }
            }
        } else if (method == "mining.set_difficulty") {
            const Value& valParams = find_value(responseObject, "params");
            if (valParams.type() == array_type) {
                const Array& params = valParams.get_array();
                m_nextJobDifficulty = params[0].get_real();
                if (m_nextJobDifficulty <= 0.0001) m_nextJobDifficulty = 0.0001;
                LogS("Difficulty set to %s\n", m_nextJobDifficulty);
            }
        } else if (method == "client.reconnect") {
            const Value& valParams = find_value(responseObject, "params");
            if (valParams.type() == array_type) {
                const Array& params = valParams.get_array();
                m_primary.host = params[0].get_str();
                m_primary.port = params[1].get_str();
                // TODO: Handle wait time
                LogS("Reconnection requested\n");
                reconnect();
            }
        }
        break;
    }
}

template <typename Miner, typename Job, typename Solution>
void StratumClient<Miner, Job, Solution>::work_timeout_handler(
        const boost::system::error_code& ec)
{
    if (!ec) {
        LogS("No new work received in %d seconds.\n", m_worktimeout);
        reconnect();
    }
}

template <typename Miner, typename Job, typename Solution>
bool StratumClient<Miner, Job, Solution>::submit(const Solution* solution)
{
    x_current.lock();
    Job* tempJob = p_current->clone();
    Job* tempPreviousJob;
    if (p_previous) {
        tempPreviousJob = p_previous->clone();
    }
    x_current.unlock();

    LogS("Solution found; Submitting to %s...\n", p_active->host);

    LogS("  %s\n", solution->toString());


    if (tempJob->evalSolution(solution)) {
        string json = "{\"id\": 4, \"method\": \"mining.submit\", \"params\": [\"" +
                      p_active->user + "\",\"" +
                      tempJob->getSubmission(solution) + "\"]}\n";
        std::ostream os(&m_requestBuffer);
        os << json;
        m_stale = false;
        write(m_socket, m_requestBuffer);
        return true;
    } else if (tempPreviousJob && tempPreviousJob->evalSolution(solution)) {
        string json = "{\"id\": 4, \"method\": \"mining.submit\", \"params\": [\"" +
                      p_active->user + "\",\"" +
                      tempPreviousJob->getSubmission(solution) + "\"]}\n";
        std::ostream os(&m_requestBuffer);
        os << json;
        m_stale = true;
        LogS("[WARN] Submitting stale solution.\n");
        write(m_socket, m_requestBuffer);
        return true;
    } else {
        m_stale = false;
        LogS("[WARN] FAILURE: Miner gave incorrect result!\n");
        p_miner->failedSolution();
    }

    return false;
}
