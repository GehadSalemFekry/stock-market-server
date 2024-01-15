
#include <boost/asio.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <cstdlib>
#include <curl/curl.h>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

namespace beast = boost::beast;         // from <boost/beast.hpp>
namespace http = beast::http;           // from <boost/beast/http.hpp>
namespace websocket = beast::websocket; // from <boost/beast/websocket.hpp>
using tcp = boost::asio::ip::tcp;       // from <boost/asio/ip/tcp.hpp>

class session : public std::enable_shared_from_this<session>
{
    websocket::stream<beast::tcp_stream> ws_;
    beast::flat_buffer buffer_;

  public:
    // Take ownership of the socket
    explicit session(tcp::socket &&socket) : ws_(std::move(socket))
    {
    }

    // Start the asynchronous operation
    void run()
    {
        // Set the CORS headers
        ws_.set_option(websocket::stream_base::decorator([](websocket::response_type &res) {
            res.set(http::field::access_control_allow_origin, "*");
            res.set(http::field::access_control_allow_credentials, "true");
            res.set(http::field::access_control_allow_methods, "GET, POST, OPTIONS");
            res.set(http::field::access_control_allow_headers, "Content-Type, Authorization");
        }));

        // Perform the WebSocket handshake
        ws_.async_accept(beast::bind_front_handler(&session::on_accept, shared_from_this()));
    }

    void on_accept(beast::error_code ec)
    {
        if (ec)
            return session::fail(ec, "accept");

        // Read a message
        do_read();
    }

    void do_read()
    {
        // Read a message into our buffer
        ws_.async_read(buffer_, beast::bind_front_handler(&session::on_read, shared_from_this()));
    }

    void on_read(beast::error_code ec, std::size_t bytes_transferred)
    {
        boost::ignore_unused(bytes_transferred);

        // Handle the error, if any
        if (ec)
            return session::fail(ec, "read");

        // Echo the message back
        ws_.text(ws_.got_text());
        ws_.async_write(buffer_.data(), beast::bind_front_handler(&session::on_write, shared_from_this()));
    }

    void on_write(beast::error_code ec, std::size_t bytes_transferred)
    {
        boost::ignore_unused(bytes_transferred);

        if (ec)
            return session::fail(ec, "write");

        // Clear the buffer
        buffer_.consume(buffer_.size());

        // Do another read
        do_read();
    }

    static void fail(beast::error_code ec, char const *what)
    {
        std::cerr << what << ": " << ec.message() << "\n";
    }

    void send(const std::string &message)
    {
        auto self = shared_from_this();
        ws_.async_write(boost::asio::buffer(message),
                        [this, self](boost::system::error_code ec, std::size_t /*length*/) {
                            if (!ec)
                            {
                                std::cout << "Message sent successfully." << std::endl;
                            }
                            else
                            {
                                std::cerr << "Error sending message: " << ec.message() << std::endl;
                            }
                        });
    }
};

class WebSocketServer
{
    std::vector<std::shared_ptr<session>> sessions_;
    boost::asio::io_context &ioc_;
    boost::asio::steady_timer poll_timer_;

  public:
    WebSocketServer(boost::asio::io_context &ioc) : ioc_(ioc), poll_timer_(ioc, boost::asio::chrono::seconds(10))
    {
        start_polling();
    }

    void add_session(std::shared_ptr<session> new_session)
    {
        sessions_.push_back(new_session);
    }

    void start_polling()
    {
        poll_timer_.expires_after(boost::asio::chrono::seconds(10));
        poll_timer_.async_wait([this](boost::system::error_code ec) {
            if (!ec)
            {
                fetch_and_broadcast_stock_data();
                start_polling(); // Restart the timer
            }
        });
    }

    void fetch_and_broadcast_stock_data()
    {
        CURL *curl = curl_easy_init();
        if (!curl)
        {
            std::cerr << "Failed to initialize CURL" << std::endl;
            return;
        }

        CURLcode res;
        std::string readBuffer;
        curl_easy_setopt(curl, CURLOPT_URL, "http://localhost:8080/stocks");
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);
        res = curl_easy_perform(curl);
        curl_easy_cleanup(curl);

        std::cout << "Number of sessions: " << sessions_.size() << std::endl;
        for (auto &s : sessions_)
        {
            s->send(readBuffer);
        }
    }

    static size_t WriteCallback(void *contents, size_t size, size_t nmemb, void *userp)
    {
        ((std::string *)userp)->append((char *)contents, size * nmemb);
        return size * nmemb;
    }
};

class listener : public std::enable_shared_from_this<listener>
{
    tcp::acceptor acceptor_;
    tcp::socket socket_;
    WebSocketServer &server_;

  public:
    listener(boost::asio::io_context &ioc, tcp::endpoint endpoint, WebSocketServer &server)
        : acceptor_(ioc), socket_(ioc), server_(server)
    {
        beast::error_code ec;

        // Open the acceptor
        acceptor_.open(endpoint.protocol(), ec);
        if (ec)
        {
            session::fail(ec, "open");
            return;
        }

        // Bind to the server address
        acceptor_.bind(endpoint, ec);
        if (ec)
        {
            session::fail(ec, "bind");
            return;
        }

        // Start listening for connections
        acceptor_.listen(boost::asio::socket_base::max_listen_connections, ec);
        if (ec)
        {
            session::fail(ec, "listen");
            return;
        }
    }

    // Start accepting incoming connections
    void run()
    {
        do_accept();
    }

  private:
    void do_accept()
    {
        acceptor_.async_accept(socket_, beast::bind_front_handler(&listener::on_accept, shared_from_this()));
    }

    void on_accept(beast::error_code ec)
    {
        if (ec)
        {
            session::fail(ec, "accept");
        }
        else
        {
            auto ws_session = std::make_shared<session>(std::move(socket_));
            server_.add_session(ws_session);
            ws_session->run();
        }

        do_accept();
    }
};

int main(int argc, char *argv[])
{
    try
    {
        // Define the server address and port
        auto const address = boost::asio::ip::make_address("127.0.0.1");
        auto const port = static_cast<unsigned short>(std::atoi("8081"));

        // Create a Boost.Asio I/O context
        boost::asio::io_context ioc{1};

        // Create the WebSocket server
        WebSocketServer server(ioc);

        // Create and start the listener
        std::make_shared<listener>(ioc, tcp::endpoint{address, port}, server)->run();

        // Run the I/O context to start asynchronous operations
        ioc.run();
    }
    catch (std::exception const &e)
    {
        // Handle any exceptions
        std::cerr << "Error: " << e.what() << "\n";
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}