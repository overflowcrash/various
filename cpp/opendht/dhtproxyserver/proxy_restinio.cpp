// Vsevolod Ivanov

#include "proxy_restinio.h"

DhtProxyServer::DhtProxyServer(std::shared_ptr<dht::DhtRunner> dhtNode,
                               in_port_t port):
    dhtNode(dhtNode), httpServer_(restinio::own_io_context(), []( auto & settings ){})
{
    this->jsonBuilder["commentStyle"] = "None";
    this->jsonBuilder["indentation"] = "";

    this->serverThread = std::thread([this, port](){
        using namespace std::chrono;
        auto maxThreads = std::thread::hardware_concurrency() - 1; // dht
        auto restThreads = maxThreads > 1 ? maxThreads : 1;
        printf("Running on restinio on %i threads\n", restThreads);
        auto settings = ServerSettings(restThreads);
        settings.address("127.0.0.1");
        settings.port(port);
        settings.request_handler(this->createRestRouter());
        settings.read_next_http_message_timelimit(10s);
        settings.write_http_response_timelimit(10s);
        settings.handle_request_timeout(10s);
        settings.socket_options_setter([](auto & options){
            options.set_option(asio::ip::tcp::no_delay{true});
        });
        httpServer_ = restinio::http_server_t<RestRouterTraits>(
            restinio::own_io_context(),
            std::forward<ServerSettings>(settings)
        );
        restinio::asio_ns::post(httpServer_.io_context(),
            [&]{
                httpServer_.open_sync();
            });
        try {
            httpServer_.io_context().run();
        }
        catch(const std::exception &ex) {
            std::cerr << "Error: " << ex.what() << std::endl;
        }
    });
}

DhtProxyServer::~DhtProxyServer()
{
    stop();
}

bool DhtProxyServer::running()
{
    return !httpServer_.io_context().stopped();
}

void DhtProxyServer::stop()
{
    httpServer_.close_sync();

    if (this->serverThread.joinable()){
        this->serverThread.join();
        this->dhtNode->join();
    }
}

std::unique_ptr<RestRouter> DhtProxyServer::createRestRouter()
{
    using namespace std::placeholders;
    auto router = std::make_unique<RestRouter>();
    router->add_handler(restinio::http_method_options(),
                        "/", std::bind(&DhtProxyServer::options, this, _1, _2));
    router->http_get("/", std::bind(&DhtProxyServer::getNodeInfo, this, _1, _2));
    router->http_get("/:hash", std::bind(&DhtProxyServer::get, this, _1, _2));
    router->http_post("/:hash", std::bind(&DhtProxyServer::put, this, _1, _2));
    return router;
}

template <typename HttpResponse>
HttpResponse DhtProxyServer::initHttpResponse(HttpResponse response)
{
    response.append_header("Server", "RESTinio");
    response.append_header(restinio::http_field::content_type, "application/json");
    response.append_header(restinio::http_field::access_control_allow_origin, "*");
    response.connection_keep_alive();
    return response;
}

request_status DhtProxyServer::options(restinio::request_handle_t request,
                                       restinio::router::route_params_t params)
{
    this->requestCount++;
#ifdef OPENDHT_PROXY_SERVER_IDENTITY
    const auto methods = "OPTIONS, GET, POST, LISTEN, SIGN, ENCRYPT";
#else
    const auto methods = "OPTIONS, GET, POST, LISTEN";
#endif
    auto response = initHttpResponse(request->create_response());
    response.append_header(restinio::http_field::access_control_allow_methods, methods);
    response.append_header(restinio::http_field::access_control_allow_headers, "content-type");
    response.append_header(restinio::http_field::access_control_max_age, "86400");
    return response.done();
}

request_status DhtProxyServer::getNodeInfo(
    restinio::request_handle_t request, restinio::router::route_params_t params)
{
    printf("Connection Id: %lu\n", request->connection_id());
    Json::Value result;
    std::lock_guard<std::mutex> lck(statsMutex);
    if (this->dhtNodeInfo.ipv4.good_nodes == 0 &&
        this->dhtNodeInfo.ipv6.good_nodes == 0){
        this->dhtNodeInfo = this->dhtNode->getNodeInfo();
    }
    result = this->dhtNodeInfo.toJson();
    // [ipv6:ipv4]:port or ipv4:port
    result["public_ip"] = request->remote_endpoint().address().to_string();
    auto output = Json::writeString(this->jsonBuilder, result) + "\n";

    auto response = this->initHttpResponse(request->create_response());
    response.append_body(output);
    return response.done();
}

request_status DhtProxyServer::get(restinio::request_handle_t request,
                                   restinio::router::route_params_t params)
{
    dht::InfoHash infoHash(params["hash"].to_string());
    if (!infoHash)
        infoHash = dht::InfoHash::get(params["hash"].to_string());

    auto response = std::make_shared<restinio::response_builder_t<response_t>>(
        this->initHttpResponse(request->create_response<response_t>()));
    response->flush();

    this->dhtNode->get(infoHash, [this, response] (const dht::Sp<dht::Value>& value){
        auto output = Json::writeString(this->jsonBuilder, value->toJson()) + "\n";
        response->append_chunk(output);
        response->flush();
        return true;
    },
    [response] (bool /*ok*/){
        response->done();
    });
    return restinio::request_handling_status_t::accepted;
}

request_status DhtProxyServer::put(restinio::request_handle_t request,
                                   restinio::router::route_params_t params)
{
    dht::InfoHash infoHash(params["hash"].to_string());
    if (!infoHash)
        infoHash = dht::InfoHash::get(params["hash"].to_string());

    if (request->body().empty()) {
        auto response = this->initHttpResponse(request->create_response(
            restinio::status_bad_request()));
        response.set_body(this->RESP_MSG_MISSING_PARAMS);
        return response.done();
    }

    std::string err;
    Json::Value root;
    Json::CharReaderBuilder rbuilder;
    auto* char_data = reinterpret_cast<const char*>(request->body().data());
    auto reader = std::unique_ptr<Json::CharReader>(rbuilder.newCharReader());

    if (reader->parse(char_data, char_data + request->body().size(), &root, &err)){
        // Build the dht::Value from json, NOTE: {"data": "base64value", ...}
        auto value = std::make_shared<dht::Value>(root);
        bool permanent = root.isMember("permanent");
        std::cout << "Got put " << infoHash << " " << *value <<
                     " " << (permanent ? "permanent" : "") << std::endl;
        /*
        if (permanent){
        }
        else {
        }
        */
        this->dhtNode->put(infoHash, value, [this, request, value](bool ok){
            if (ok){
                Json::StreamWriterBuilder wbuilder;
                wbuilder["commentStyle"] = "None";
                wbuilder["indentation"] = "";
                auto output = Json::writeString(this->jsonBuilder, value->toJson()) + "\n";
                std::cout << output << std::endl;
                auto response = this->initHttpResponse(request->create_response());
                response.append_body(output);
                response.done();
            } else {
                auto response = this->initHttpResponse(request->create_response(
                    restinio::status_bad_gateway()));
                response.set_body(this->RESP_MSG_PUT_FAILED);
                response.done();
            }

        }, dht::time_point::max(), permanent);
    }
    return restinio::request_handling_status_t::accepted;
}

int main()
{
    auto dhtNode = std::make_shared<dht::DhtRunner>();
    dhtNode->run(4444, dht::crypto::generateIdentity(), true);
    dhtNode->bootstrap("bootstrap.jami.net", "4222");

    DhtProxyServer dhtproxy {dhtNode, 8080};
    while (dhtproxy.running()) {
        std::this_thread::sleep_for(std::chrono::seconds(10));
        //printf("stopping..\n");
        //dhtproxy.stop();
    };
}
