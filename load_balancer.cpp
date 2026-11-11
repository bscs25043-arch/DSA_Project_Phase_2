// ======================= TIME-TRAVEL DEBUGGER - LOAD BALANCER TEMPLATE =======================

// What load balancer does:
//   1. Many clients can connect at the same time. Each one sends one string (the source code). 
//   2. Clients wait in a queue (first come, first served).
//   3. For each client, in order: make sure the server is running (start it with system() if not),
//      pass the string to the server, pass the server's reply back to the client.
//
// It does not read or understand the messages. It only moves bytes.
// The only messages it writes itself are errors ("server busy", "server failed to start").
//
// Two threads:
//   acceptor thread (acceptLoop) -> takes new clients and puts them in the queue. It never waits for the server.
//   main thread (dispatcherLoop) -> takes ONE client out of the queue and serves it completely, then the next.
//
// How the server is started:
//   system("./server 5001 &");
//   The "&" is required. Without it, system() waits until the server exits and the load balancer freezes.
//
//
// What the server program must do (the load balancer depends on this):
//   - it takes its port as the first argument, for example: ./server 5001
//   - it reads the string until the connection's write side is closed (recv returns 0)
//   - it sends its reply as [status: 1 byte][payload], then closes the connection 


#include <iostream>
#include <string>
#include <cstring>
#include <cstdlib>
#include <thread>
#include <mutex>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "helper.cpp"
using namespace std;

// ---- Constants ----
const char *SERVER_PATH = "./server";      // the program we start when the server is down
const int IO_BUFFER_SIZE = 64 * 1024;      // fixed buffer for moving bytes
const int LISTEN_BACKLOG = 16;             // connections the OS holds until accept() takes them
const int MAX_PENDING_CLIENTS = 8;         // clients allowed to wait in our queue. More than this get "server busy".
const int STARTUP_MAX_ATTEMPTS = 50;       // after starting the server, how many times we try to connect
const int STARTUP_POLL_INTERVAL_MS = 100;  // wait between those tries (50 x 100 ms = 5 seconds in total)
int lbPort = 5000;
string serverHost = "127.0.0.0";
int serverPort = 5001;


// ---- Queue: here it holds client sockets (int). ----
// Implement it yourself, do not use std::queue.
template <typename T>
class Queue
{
    struct Node
    {
        T data;
        Node *next;
        Node(const T &val) : data(val), next(nullptr) {}
    };
    Node *front, *rear;
    int count;

public:
    Queue()
    {
        front = nullptr;
        rear = nullptr;
        count = 0;
    }

    ~Queue()
    {
        while (!isEmpty())
        {
            dequeue();
        }
    }

    void enqueue(const T &val)
    {
        Node *nEw = new Node(val);
        if (count == 0)
        {
            front = nEw;
            rear = front;
        }
        else
        {
            rear->next = nEw;
            rear = nEw;
        }
        count++;
    }

    T dequeue()
    {
        if (isEmpty())
        {
            std::__throw_out_of_range("THE QUEUE IS EMPTY");
        }

        Node *tempNode = front;
        T temp = tempNode->data;

        front = front->next;
        delete tempNode;
        count--;

        if (count == 0)
        {
            rear = nullptr;
        }

        return temp;
    }

    bool isEmpty()
    {
        return count == 0;
    }

    int size()
    {
        return count;
    }
};
struct ClientInfo
{
    int sock;           // the connected socket
    string address;     // the client's IP as text, e.g. "127.0.0.1"
    int port;           // the client's port
    time_t arrivedAt;   // when accept() returned
};

// The queue is used by two threads, so every use of it must be inside queueMutex.
Queue<ClientInfo> pendingClients;
mutex queueMutex;
sig_atomic_t flag = 1;


// ---- Basic socket helpers ----
bool sendAll(int sock, const char *buf, size_t n)
{
    size_t sent = 0;
    while (sent < n)
    {
        ssize_t res = send(sock, buf + sent, n - sent, 0);
        if (res <= 0)
        {
            return false; 
        }
        sent = sent + res;
    }
    return true;
}
bool sendError(int sock, const string &message)
{
    char stat_bite = 1; 
    if (!sendAll(sock, &stat_bite, 1))
    {
        return false;
    }
    return sendAll(sock, message.c_str(), message.length());
}

int createListenSocket(int port)
{
    int sock = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (sock < 0)
    {
        cerr << "Error creating listen socket" << endl;
        exit(1);
    }

    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(static_cast<uint16_t>(port));

    if (bind(sock, (sockaddr *)&addr, sizeof(addr)) < 0)
    {
        cerr << "Error binding listen socket to port " << port << endl;
        close(sock);
        exit(1);
    }

    if (listen(sock, LISTEN_BACKLOG) < 0)
    {
        cerr << "Error listening on socket" << endl;
        close(sock);
        exit(1);
    }
    return sock;
}

int connectToServer(const char *host, int port)
{
    int sock = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (sock < 0)
    {
        return -1;
    }

    sockaddr_in addr= {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));

    if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0)
    {
        close(sock);
        return -1;
    }

    if (connect(sock, (sockaddr *)&addr, sizeof(addr)) < 0)
    {
        close(sock);
        return -1;
    }

    return sock;
}

// =============================================================================================
// PART 1: WAKING UP THE SERVER IF IT IS DOWN
// =============================================================================================


bool startServer(int port)
{
    // Build the command text: SERVER_PATH + " " + port as text + "&"
    //   Example: "./server 5001 &"
    // Call system() with it.
    // Return true if system() returned 0, otherwise false.
    string cmd = string(SERVER_PATH) + " " + to_string(port) + " &";
    int res = system(cmd.c_str());
    return (res == 0);
}

int connectToServerWithWakeup()
{
    int sock = connectToServer(serverHost.c_str(), serverPort);
    if (sock >= 0)
    {
        return sock;
    }

    if (!startServer(serverPort))
    {
        return -1;
    }

  
    for (int i = 0; i < STARTUP_MAX_ATTEMPTS; i++)
    {
        usleep(STARTUP_POLL_INTERVAL_MS * 1000);
        sock = connectToServer(serverHost.c_str(), serverPort);
        if (sock >= 0)
        {
            return sock; 
        }
    }

    return -1; 
}


// =============================================================================================
// PART 2: PASSING THE STRING AND THE REPLY (one client, called by the main thread only)
// =============================================================================================


void relayOneClient(ClientInfo &c)
{
   
    time_t now = time(nullptr);
    if (difftime(now, c.arrivedAt) > 30.0)
    {
        close(c.sock);
        return;
    }
    int serverSock = connectToServerWithWakeup();
    if (serverSock < 0)
    {
        sendError(c.sock, "backend server failed to start");
        close(c.sock);
        return;
    }

    char buffer[IO_BUFFER_SIZE];

   while (true)
    {
        ssize_t bytesRead = recv(c.sock, buffer, IO_BUFFER_SIZE, 0);
        if (bytesRead <= 0)
        {
            break; 
        }

        if (!sendAll(serverSock, buffer, bytesRead))
        {
            close(serverSock);
            close(c.sock);
            return;
        }
    }

    shutdown(serverSock, SHUT_WR);

    while (true)
    {
        ssize_t bytesRead = recv(serverSock, buffer, IO_BUFFER_SIZE, 0);
        if (bytesRead <= 0)
        {
            break; 
        }

        if (!sendAll(c.sock, buffer, bytesRead))
        {
            break;
        }
    }
 
    close(serverSock);
    close(c.sock);
}

// =============================================================================================
// PART 3: THE TWO THREADS
// =============================================================================================

void acceptLoop(int listenSock)
{
    while (flag)
    {
        sockaddr_in clientAddr;
        socklen_t addrLen = sizeof(clientAddr);
        int clientSock = accept(listenSock, (sockaddr*)&clientAddr, &addrLen);
        if (clientSock < 0) continue;

        char ipStr[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &clientAddr.sin_addr, ipStr, sizeof(ipStr));

        lock_guard<mutex> lock(queueMutex);
        if (pendingClients.size() >= MAX_PENDING_CLIENTS)
        {
            sendError(clientSock, "server busy, try again later");
            close(clientSock);
        }
        else
        {
            pendingClients.enqueue({clientSock, string(ipStr), ntohs(clientAddr.sin_port), time(nullptr)});
        }
    }
}

void dispatcherLoop()
{
    while (flag)
    {
        ClientInfo client{-1, "", 0, 0};
        {
            lock_guard<mutex> lock(queueMutex);
            if (!pendingClients.isEmpty())
            {
                client = pendingClients.dequeue();
            }
        }

        if (client.sock >= 0)
        {
            relayOneClient(client);
        }
        else
        {
            usleep(10000); 
        }
    }
}

void handler(int sg) {
    (void)sg;
    flag = 0;
}

int main(int argc, char *argv[])
{
    if (argc > 1) serverHost = argv[1];
    if (argc > 2) serverPort = atoi(argv[2]);

    signal(SIGINT, handler);

    cout << "[Load Balancer] Listening on port " << lbPort 
         << ", targeting server on " << serverHost << ":" << serverPort << endl;

    int listenSock = createListenSocket(lbPort);

    thread acceptor(acceptLoop, listenSock);
    acceptor.detach();

    dispatcherLoop();

    close(listenSock);
    return 0;
}