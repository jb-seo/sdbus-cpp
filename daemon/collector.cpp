/* Deprecated method call collector using UDS + D-Bus name cache. */

#include <sdbus-c++/sdbus-c++.h>

#include <curl/curl.h>

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <unistd.h>

#include <poll.h>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <net/if.h>

#include <cerrno>
#include <chrono>
#include <ctime>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <deque>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

constexpr const char* kEnvUdsPath = "SDBUSCPP_DEPRECATED_UDS_PATH";
constexpr const char* kEnvElasticUrl = "SDBUSCPP_ELASTIC_URL";
constexpr const char* kEnvCollectorBus = "SDBUSCPP_COLLECTOR_BUS";
constexpr const char* kDefaultUdsPath = "/run/sdbus-deprecated.sock";
constexpr const char* kEnvXdgRuntimeDir = "XDG_RUNTIME_DIR";
constexpr const char* kDefaultElasticUrl = "http://localhost:9200/sdbus-deprecated/_doc";

struct Event
{
    std::string sender;
    std::string interfaceName;
    std::string objectPath;
    std::string methodName;
    long pid{-1};
};

class NameOwnerCache
{
public:
    void update(const std::string& name, const std::string& oldOwner, const std::string& newOwner)
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!oldOwner.empty())
        {
            auto it = ownerToNames_.find(oldOwner);
            if (it != ownerToNames_.end())
            {
                it->second.erase(name);
                if (it->second.empty())
                    ownerToNames_.erase(it);
            }
        }

        if (newOwner.empty())
        {
            nameToOwner_.erase(name);
            return;
        }

        nameToOwner_[name] = newOwner;
        ownerToNames_[newOwner].insert(name);
    }

    std::string getNameForOwner(const std::string& owner) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = ownerToNames_.find(owner);
        if (it == ownerToNames_.end())
            return "-";
        for (const auto& name : it->second)
        {
            if (!name.empty() && name[0] != ':')
                return name;
        }
        return "-";
    }

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::unordered_set<std::string>> ownerToNames_;
    std::unordered_map<std::string, std::string> nameToOwner_;
};

class ElasticSink
{
public:
    explicit ElasticSink(std::string url)
        : url_(std::move(url))
    {
    }

    void enqueue(std::string payload)
    {
        pending_.push_back(std::move(payload));
        flush("enqueue");
    }

    void flushPending()
    {
        if (pending_.empty())
            return;
        flush("periodic");
    }

    size_t pendingSize() const
    {
        return pending_.size();
    }

private:
    static std::string jsonEscape(const std::string& input)
    {
        std::string out;
        out.reserve(input.size());
        for (char c : input)
        {
            switch (c)
            {
                case '\\': out += "\\\\"; break;
                case '"': out += "\\\""; break;
                case '\b': out += "\\b"; break;
                case '\f': out += "\\f"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20)
                        out += "?";
                    else
                        out += c;
                    break;
            }
        }
        return out;
    }


    bool sendPayload(const std::string& payload)
    {
        CURL* curl = curl_easy_init();
        if (!curl)
            return false;

        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_URL, url_.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, payload.size());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &ElasticSink::discardResponse);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 2000L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

        CURLcode res = curl_easy_perform(curl);
        long httpCode = 0;
        if (res == CURLE_OK)
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        return (res == CURLE_OK && (httpCode == 200 || httpCode == 201));
    }

    void flush(const char* reason)
    {
        if (!pending_.empty())
            std::cout << "[elastic] attempt reason=" << reason << " queued=" << pending_.size() << "\n";

        while (!pending_.empty())
        {
            if (!sendPayload(pending_.front()))
            {
                std::cout << "[elastic] send failed, queued=" << pending_.size() << "\n";
                break;
            }
            pending_.pop_front();
        }

        if (pending_.empty())
            std::cout << "[elastic] queue empty\n";
    }

    std::string url_;
    std::deque<std::string> pending_;

public:
    std::string buildPayload(const Event& ev,
                             const std::string& name,
                             const std::string& exePath,
                             const std::string& cmdline,
                             const std::string& timestamp,
                             const std::string& hostIp)
    {
        std::ostringstream os;
        os << "{"
           << "\"@timestamp\":\"" << jsonEscape(timestamp) << "\","
           << "\"host_ip\":\"" << jsonEscape(hostIp) << "\","
           << "\"sender\":\"" << jsonEscape(ev.sender) << "\","
           << "\"name\":\"" << jsonEscape(name) << "\","
           << "\"interface\":\"" << jsonEscape(ev.interfaceName) << "\","
           << "\"object\":\"" << jsonEscape(ev.objectPath) << "\","
           << "\"method\":\"" << jsonEscape(ev.methodName) << "\","
           << "\"pid\":" << ev.pid << ","
           << "\"exe\":\"" << jsonEscape(exePath) << "\","
           << "\"cmdline\":\"" << jsonEscape(cmdline) << "\""
           << "}";
        return os.str();
    }

    static size_t discardResponse(char* ptr, size_t size, size_t nmemb, void* userdata)
    {
        (void)ptr;
        (void)userdata;
        return size * nmemb;
    }
};

std::string getSocketPath()
{
    const char* env = std::getenv(kEnvUdsPath);
    if (env && *env != '\0' && std::strcmp(env, "0") != 0)
        return env;
    const char* runtimeDir = std::getenv(kEnvXdgRuntimeDir);
    if (runtimeDir && *runtimeDir != '\0')
        return std::string(runtimeDir) + "/sdbus-deprecated.sock";
    return kDefaultUdsPath;
}

std::string getElasticUrl()
{
    const char* env = std::getenv(kEnvElasticUrl);
    if (env && *env != '\0' && std::strcmp(env, "0") != 0)
        return env;
    return kDefaultElasticUrl;
}

std::string getCollectorBus()
{
    const char* env = std::getenv(kEnvCollectorBus);
    if (env && *env != '\0')
        return env;
    return "auto";
}

bool parseLine(const std::string& line, Event& out)
{
    std::istringstream iss(line);
    std::string pidStr;
    if (!(iss >> out.sender >> out.interfaceName >> out.objectPath >> out.methodName >> pidStr))
        return false;

    try
    {
        out.pid = std::stol(pidStr);
    }
    catch (...)
    {
        out.pid = -1;
    }

    return true;
}

std::string readExePath(long pid)
{
    if (pid <= 0)
        return {};

    std::string linkPath = "/proc/" + std::to_string(pid) + "/exe";
    char buf[4096];
    ssize_t n = readlink(linkPath.c_str(), buf, sizeof(buf) - 1);
    if (n <= 0)
        return {};
    buf[n] = '\0';
    return std::string(buf);
}

std::string readCmdline(long pid)
{
    if (pid <= 0)
        return {};

    std::string path = "/proc/" + std::to_string(pid) + "/cmdline";
    FILE* fp = std::fopen(path.c_str(), "rb");
    if (!fp)
        return {};

    std::string data;
    char buf[1024];
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), fp)) > 0)
        data.append(buf, n);
    std::fclose(fp);

    if (data.empty())
        return {};

    for (char& c : data)
    {
        if (c == '\0')
            c = ' ';
    }

    while (!data.empty() && data.back() == ' ')
        data.pop_back();

    return data;
}

std::string getTimestampUtc()
{
    std::time_t now = std::time(nullptr);
    std::tm tmUtc{};
    gmtime_r(&now, &tmUtc);
    char buf[32];
    if (std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tmUtc) == 0)
        return {};
    return std::string(buf);
}

std::string getHostIp()
{
    ifaddrs* ifaddr = nullptr;
    if (getifaddrs(&ifaddr) != 0)
        return "-";

    auto isPreferredName = [](const char* name) {
        if (!name)
            return false;
        return std::strncmp(name, "en", 2) == 0 ||
               std::strncmp(name, "eth", 3) == 0 ||
               std::strncmp(name, "wl", 2) == 0;
    };

    std::string fallback = "-";
    for (int pass = 0; pass < 2; ++pass)
    {
        for (ifaddrs* it = ifaddr; it != nullptr; it = it->ifa_next)
        {
            if (!it->ifa_addr || it->ifa_addr->sa_family != AF_INET)
                continue;
            if ((it->ifa_flags & IFF_LOOPBACK) != 0)
                continue;
            if ((it->ifa_flags & IFF_POINTOPOINT) != 0)
                continue;
            if ((it->ifa_flags & IFF_UP) == 0)
                continue;
            if ((it->ifa_flags & IFF_RUNNING) == 0)
                continue;
            if (pass == 0 && !isPreferredName(it->ifa_name))
                continue;

            auto* addr = reinterpret_cast<sockaddr_in*>(it->ifa_addr);
            char buf[INET_ADDRSTRLEN];
            if (!inet_ntop(AF_INET, &addr->sin_addr, buf, sizeof(buf)))
                continue;
            if (std::strncmp(buf, "127.", 4) == 0)
                continue;
            fallback = buf;
            break;
        }
        if (fallback != "-")
            break;
    }

    freeifaddrs(ifaddr);
    return fallback;
}

void handleEvent(const Event& ev, const NameOwnerCache& cache, ElasticSink& sink)
{
    const std::string name = cache.getNameForOwner(ev.sender);

    const std::string exePath = readExePath(ev.pid);
    const std::string cmdline = readCmdline(ev.pid);
    const std::string timestamp = getTimestampUtc();
    static const std::string hostIp = getHostIp();

    std::cout << "[" << (timestamp.empty() ? "-" : timestamp) << "] Deprecated - "
              << "sender=" << ev.sender
              << " name=" << name
              << " interface=" << ev.interfaceName
              << " object=" << ev.objectPath
              << " method=" << ev.methodName
              << " pid=" << ev.pid
              << " exe=" << (exePath.empty() ? "-" : exePath)
              << "\n";
    std::cout.flush();

    sink.enqueue(sink.buildPayload(ev, name, exePath, cmdline, timestamp, hostIp));
}

void seedNameCache(sdbus::IConnection& connection, NameOwnerCache& cache)
{
    auto proxy = sdbus::createProxy(connection,
                                    sdbus::ServiceName{"org.freedesktop.DBus"},
                                    sdbus::ObjectPath{"/org/freedesktop/DBus"});

    std::vector<std::string> names;
    proxy->callMethod("ListNames")
        .onInterface("org.freedesktop.DBus")
        .storeResultsTo(names);

    for (const auto& name : names)
    {
        try
        {
            std::string owner;
            proxy->callMethod("GetNameOwner")
                .onInterface("org.freedesktop.DBus")
                .withArguments(name)
                .storeResultsTo(owner);
            cache.update(name, std::string{}, owner);
        }
        catch (...)
        {
        }
    }
}

} // namespace

int main(int argc, char** argv)
{
    const std::string busMode = (argc > 1) ? argv[1] : getCollectorBus();
    const std::string socketPath = (argc > 2) ? argv[2] : getSocketPath();
    const std::string elasticUrl = (argc > 3) ? argv[3] : getElasticUrl();
    curl_global_init(CURL_GLOBAL_DEFAULT);

    int serverFd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (serverFd < 0)
    {
        std::cerr << "Failed to create socket\n";
        return 1;
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (socketPath.size() >= sizeof(addr.sun_path))
    {
        std::cerr << "Socket path too long\n";
        return 1;
    }
    std::strncpy(addr.sun_path, socketPath.c_str(), sizeof(addr.sun_path) - 1);

    umask(0);
    unlink(socketPath.c_str());
    if (bind(serverFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
    {
        std::cerr << "Failed to bind socket: " << std::strerror(errno) << "\n";
        return 1;
    }

    if (listen(serverFd, 16) != 0)
    {
        std::cerr << "Failed to listen on socket\n";
        return 1;
    }

    NameOwnerCache cache;
    ElasticSink sink(elasticUrl);
    std::unique_ptr<sdbus::IConnection> connection;
    if (busMode == "session")
    {
        connection = sdbus::createSessionBusConnection();
        std::cerr << "[collector] bus=session\n";
    }
    else if (busMode == "system")
    {
        connection = sdbus::createSystemBusConnection();
        std::cerr << "[collector] bus=system\n";
    }
    else
    {
        connection = sdbus::createBusConnection();
        std::cerr << "[collector] bus=auto\n";
    }
    connection->addMatch(
        "type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',member='NameOwnerChanged'",
        [&cache](sdbus::Message msg) {
            std::string name;
            std::string oldOwner;
            std::string newOwner;
            msg >> name >> oldOwner >> newOwner;
            cache.update(name, oldOwner, newOwner);
        });

    try
    {
        seedNameCache(*connection, cache);
    }
    catch (...)
    {
        std::cerr << "[collector] seedNameCache failed\n";
    }

    connection->enterEventLoopAsync();

    for (;;)
    {
        pollfd pfd{};
        pfd.fd = serverFd;
        pfd.events = POLLIN;

        int pr = poll(&pfd, 1, 10000);
        if (pr == 0)
        {
            if (sink.pendingSize() > 0)
                sink.flushPending();
            continue;
        }
        if (pr < 0)
        {
            if (errno == EINTR)
                continue;
            std::cerr << "Poll error\n";
            break;
        }

        int clientFd = accept(serverFd, nullptr, nullptr);
        if (clientFd < 0)
        {
            if (errno == EINTR)
                continue;
            std::cerr << "Failed to accept connection\n";
            break;
        }

        std::string buffer;
        char chunk[1024];
        ssize_t n = 0;
        while ((n = read(clientFd, chunk, sizeof(chunk))) > 0)
        {
            buffer.append(chunk, static_cast<size_t>(n));
            size_t pos = 0;
            while ((pos = buffer.find('\n')) != std::string::npos)
            {
                std::string line = buffer.substr(0, pos);
                buffer.erase(0, pos + 1);
                if (line.empty())
                    continue;
                Event ev;
                if (parseLine(line, ev))
                    handleEvent(ev, cache, sink);
            }
        }

        close(clientFd);
    }

    close(serverFd);
    unlink(socketPath.c_str());
    curl_global_cleanup();
    return 0;
}
