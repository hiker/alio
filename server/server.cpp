//
//    ALIO - ALternative IO library
//    Copyright (C) 2013  Joerg Henrichs
//
//    ALIO is free software: you can redistribute it and/or modify
//    it under the terms of the GNU General Public License as published by
//    the Free Software Foundation, either version 3 of the License, or
//    (at your option) any later version.
//
//    ALIO is distributed in the hope that it will be useful,
//    but WITHOUT ANY WARRANTY; without even the implied warranty of
//    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//    GNU General Public License for more details.
//
//    You should have received a copy of the GNU General Public License
//    along with ALIO.  If not, see <http://www.gnu.org/licenses/>.
//


#include "server/server.hpp"
#include "tools/i_communication.hpp"
#include "tools/message.hpp"
#include "tools/os.hpp"

#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <pthread.h>
#include <pwd.h>
#include <sstream>
#include <stdlib.h>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

/** Starts the server. The server listens on a socket in a separate thread
 *  for connection requests. The socket will send the connection information
 *  back to the client (e.g. MPI port string), and inform the main thread
 *  to accept a new incomming connection. This is a work around for MPI's
 *  blocking MPI_Accept call :(
 *
 *  \param if_name Name of the interface on which to listen.
 *  \param communication A communication object to communicate with a client.
 */
Server::Server(const std::string &if_name, ICommunication *communication )
{
    m_config_dir    = ALIO::OS::getConfigDir();
    m_file          = NULL;
    m_filedes       = 0;
    m_communication = communication;
    m_if_name       = if_name;
    m_new_connection_available.setAtomic(0);

    // Spawn of a thread that will handle incoming connection requests.
    // This is necessary e.g. in the case of MPI since the MPI_Comm_accept
    // blocks (and then this process can not do anything else, and trying
    // to use multi-threading with (esp Open)MPI is a bad idea.
    pthread_attr_t  attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
    // Should be the default, but just in case:
    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
    pthread_t *thread_id = new pthread_t();


    int error = pthread_create(thread_id, &attr,
                               &Server::handleConnectionRequests,  this);

    sleep(5000);
    m_communication->openPort();

    FILE *port_file = ALIO::OS::fopen("alio_config.dat", "w");
    char *port_name = m_communication->getPortName();

    ALIO::OS::fwrite(port_name, 1, strlen(port_name), port_file);
    ALIO::OS::fclose(port_file);


    while(1)
    {
        if(m_new_connection_available.getAtomic()==1)
        {
            m_communication->waitForConnection();
            m_new_connection_available.setAtomic(2);

        }
        else if (m_new_connection_available.getAtomic()==2)
        {
            if(m_communication->waitForMessage())
            {
                printf("Error waiting for message.\n");
                return;
            }
            char *buffer = m_communication->receive();
            handleRequest(buffer);
        }   // new_connection_available==2

    }   // while

}   // Server

// ----------------------------------------------------------------------------
/** Static function running with its own thread. It just calls an internal
 *  non-static function, which makes sure that 'this' is properly defined.
 */
void *Server::handleConnectionRequests(void *obj)
{
    Server *server = (Server*)obj;
    server->_handleConnectionRequests();
}   // handleConnectionRequests

// -----------------------------------------------------------------------------
/** Internal non-static function, called from handleConnection(), so that
 *  'this' is defined here.
 */
void Server::_handleConnectionRequests()
{

    // Set up the socket
    // -----------------
    int sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0) 
    {
        perror("Problems opening socket, aborting");
    }
    
    int port = 2705;
        
    struct sockaddr_in serv_addr;
    serv_addr.sin_family      = AF_INET;
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    serv_addr.sin_port        = htons(port);
        
    if(bind(sock_fd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0)
    {
        perror("Problems binding");
    }
        
    listen(sock_fd, 5);


    // Search for the right IP address for the requested interface
    // -----------------------------------------------------------
    struct ifaddrs *interface;
    getifaddrs(&interface);

    while(interface)
    {
        if(interface->ifa_addr && interface->ifa_addr->sa_family == AF_INET &&
           interface->ifa_name == m_if_name)
        {
            break;
        }
        interface = interface->ifa_next;
    }

    if(!interface)
    {
        printf("Could not find interface '%s'.\n",  m_if_name.c_str());
        perror("Aborting");
    }

    // Now write our ip address and port number into a file, which is read by the
    // clients to detect how to connect to this server
    // --------------------------------------------------------------------------
    struct sockaddr_in *p = (struct sockaddr_in *)interface->ifa_addr;
    std::ostringstream os;
    FILE *port_file = ALIO::OS::fopen( (m_config_dir+"server.dat").c_str(), "w");
    os << inet_ntoa(p->sin_addr) << " " << port;
    ALIO::OS::fwrite(os.str().c_str(), os.str().size(), 1, port_file);
    ALIO::OS::fclose(port_file);


    // Now wait for incomming connections
    // ----------------------------------
    while(1)
    {
        printf("HandleConnectionRequests is waiting now.\n");

        struct sockaddr_in client;
        socklen_t client_len = sizeof(client);
        int new_connection = accept(sock_fd, 
                                    (struct sockaddr *) &client, 
                                    &client_len);

        if(new_connection<0)
        {
            perror("Problems accepting.");
        }
        printf("Connection established.\n");

        char buffer[256];
        int n = read(new_connection, buffer, 255);
        if(n<0) 
            perror("Reading from socket.");
        
        printf("Found new connection.\n");
        char *port_name = m_communication->getPortName();
        char *p         = port_name;
        int bytes_to_write = strlen(port_name);
        
        while(bytes_to_write>0)
        {
            n = write(new_connection, port_name, bytes_to_write);
            if(n>=0)
            {
                bytes_to_write -= n;
                p += n;
            }
            else
            {
                printf("Error writing");
                perror(NULL);
            }
        }
        m_new_connection_available.setAtomic(1);
        close(new_connection);
    }   // while(1)

    close(sock_fd);
}   // handleConnectionRequests

// ----------------------------------------------------------------------------
/** Waits in a loop for incomming requests.
 *  \return 0 If a quit request was received, 1 in case of error.
 */
int Server::handleRequest(char *buffer)
{
    static int xxx=1;

    int len = m_communication->getMessageLength();
    printf("Command %d\n", (Message::MessageType)buffer[0]);
    switch((Message::MessageType)buffer[0])
    {
    case Message::MSG_FOPEN:
        {
            std::string mode;
            Message_fopen m(buffer, len, &m_filename, &mode);
            m_file = fopen(m_filename.c_str(), mode.c_str());
            break;
        }
    case Message::MSG_FOPEN64:
        {

            std::string mode;
            Message_fopen64 m(buffer, len, &m_filename, &mode);
            printf("Open64 '%s' mode '%s'\n", m_filename.c_str(), mode.c_str());
            m_file = fopen64(m_filename.c_str(), mode.c_str());
            printf("done Open64 '%s' mode '%s'\n", m_filename.c_str(), mode.c_str());
            break;
        }
    case Message::MSG_FSEEK:
        {
#define FSEEK(NAME, TYPE, MESSAGE_TYPE)                                 \
            TYPE offset;                                                \
            int whence;                                                 \
            MESSAGE_TYPE m(buffer, len, &offset, &whence);              \
            int send_len = m.getSize(whence)+m.getSize(errno);          \
            int result = NAME(m_file, offset, whence);                  \
            Message_fseek_answer answer(m.getIndex(), result, errno);   \
            answer.send(m_communication);

            FSEEK(fseek, long, Message_fseek_long);
            break;
        }   // switch

    case Message::MSG_FSEEKO:
        {
            FSEEK(fseeko, off_t, Message_fseek_off_t);
            break;
        }   // switch

    case Message::MSG_FSEEKO64:
        {
            FSEEK(fseeko64, off64_t, Message_fseek_off64_t);
            break;
        }   // switch

    case Message::MSG_FTELL:
        {
#define FTELL(MESSAGE, NAME, TYPE)                             \
            printf("XXXXXXXXX\n");\
            MESSAGE m(buffer, len);                            \
            TYPE result;                                       \
            int send_len = m.getSize(result)+m.getSize(errno); \
            printf("send len %d\n", send_len); \
            char *msg    = new char[send_len];                 \
            TYPE *p      = (TYPE*)msg;                         \
            p[0]         = NAME(m_file);                       \
            memcpy(p+1, &errno, sizeof(errno));                \
            m_communication->send(msg, send_len, 9);           \
            delete [] msg;

            FTELL(Message_ftell, ftell, long);
            break;
        }   // switch

    case Message::MSG_FTELLO:
        {
            FTELL(Message_ftello, ftello, off_t);
            break;
        }   // switch

    case Message::MSG_FTELLO64:
        {
            FTELL(Message_ftello64, ftello64, off64_t);
            break;
        }   // switch

    case Message::MSG_FERROR:
        {
            Message_ferror m(buffer, len);
            int n;
            int send_len = m.getSize(n)+m.getSize(errno);
            char *msg    = new char[send_len];
            int *p       = (int*)msg;
            if(xxx==1)
            {
                FILE *f1= fopen("Baxxaxx", "w");
                fwrite("Hello\n", 1, 6, f1);
                fclose(f1);
                xxx++;
            }
            else
            {
                FILE *f1= fopen("Caxxaxx", "w");
                fwrite("Hello\n", 1, 6, f1);
                fclose(f1);
                xxx++;
            }

            p[0]         = ferror(m_file);
            p[1]         = errno;
            m_communication->send(msg, send_len, 9);
            delete [] msg;
            break;
        }   // switch

    case Message::MSG_FWRITE:
        {
            size_t size, nmemb;
            void *data;
            Message_fwrite m(buffer, len, &size, &nmemb, &data);
            fwrite(data, size, nmemb, m_file);
            break;
        }
    case Message::MSG_FREAD:
        {
            size_t size, nmemb;
            Message_fread m(buffer, len, &size, &nmemb);

            size_t result = -1;
            int answer_size = size*nmemb+sizeof(result);
            Message_fread_answer m_ans(m.getIndex(), /*dont_allocate*/true);
            m_ans.allocate(size*nmemb+sizeof(size_t));
            // Read the file content and write it to the message buffer.
            // Leave space for the result at the beginning, which is necessary
            // for the server side to know how many bytes to copy
            result = fread(m_ans.get()+sizeof(result), size, nmemb, m_file);
            m_ans.add(result);
            m_ans.send(m_communication);
            // Memory in m_ans will be freed when m_ans is freed automatically
            break;
        }
    case Message::MSG_FCLOSE:
        {
            Message_fclose m(buffer, len);
            fclose(m_file);
            break;
        }
    case Message::MSG_FEOF:
        {
            Message_feof m(buffer, len);
            assert(false);  // not yet implemented
            break;
        }
    case Message::MSG_FGETS:
        {
            int size;
            Message_fgets m(buffer, len, &size);
            assert(false);  // not yet implemented
            break;
        }
    case Message::MSG_OPEN:
    case Message::MSG_OPEN64:
        {
            int flags;
            mode_t mode;
            Message_open m(buffer, len, &m_filename, &flags, &mode);
            if(m.getType()==Message::MSG_OPEN)
                m_filedes = open(m_filename.c_str(), flags, mode);
            else
                m_filedes = open64(m_filename.c_str(), flags, mode);
            break;
        }
    case Message::MSG___XSTAT:
        {
#ifdef XX
#define XSTAT(TYPE, FUNC)                          \
            Message_stat m(buffer, len);      \
            int send_len = sizeof(TYPE) + 2*sizeof(int);
            char *msg    = new char[send_len];
            int *p       = (int *)msg;
            p[0]         = FUNC(m_filename.c_str(), (struct stat*)(p+2) );
            p[1]         = errno;
            m_communication->send(msg, sizeof(struct stat)+2*sizeof(int), 9);
            delete [] msg;
            return false;
            XSTAT(struct stat, );
#endif
            Message_stat m(buffer, len);
            int send_len = sizeof(struct stat) + 2*sizeof(int);
            char *msg    = new char[send_len];
            int *p       = (int *)msg;
            p[0]         = stat(m_filename.c_str(), (struct stat*)(p+2) );
            p[1]         = errno;
            m_communication->send(msg, sizeof(struct stat)+2*sizeof(int), 9);
            delete [] msg;
            break;
        }
        
    case Message::MSG___FXSTAT:
        {
            Message_stat m(buffer, len);
            int send_len = sizeof(struct stat) + 2*sizeof(int);
            char *msg    = new char[send_len];
            int *p       = (int *)msg;
            p[0]         = fstat(m_filedes, (struct stat*)(p+2) );
            p[1]         = errno;
            m_communication->send(msg, sizeof(struct stat)+2*sizeof(int), 9);
            delete [] msg;
            break;
        }

    case Message::MSG___FXSTAT64:
        {
            Message_stat m(buffer, len);
            int send_len = sizeof(struct stat64) + 2*sizeof(int);
            char *msg    = new char[send_len];
            int *p       = (int *)msg;
            p[0]         = fstat64(m_filedes, (struct stat64*)(p+2) );
            p[1]         = errno;
            m_communication->send(msg, sizeof(struct stat64)+2*sizeof(int), 9);
            delete [] msg;
            break;
        }
        
    case Message::MSG___LXSTAT:
        {
            Message_stat m(buffer, len);
            int send_len = sizeof(struct stat) + 2*sizeof(int);
            char *msg    = new char[send_len];
            int *p       = (int *)msg;
            p[0]         = lstat(m_filename.c_str(), (struct stat*)(p+2) );
            p[1]         = errno;
            m_communication->send(msg, sizeof(struct stat)+2*sizeof(int), 9);
            delete [] msg;
            break;
        }
        
    case Message::MSG_LSEEK:
        {
            off_t offset;
            int whence;
            Message_lseek_off_t m(buffer, len, &offset, &whence);
            int send_len = m.getSize(offset)+m.getSize(errno);
            char *msg    = new char[send_len];
            off_t *p     = (off_t*)msg;
            p[0]         = lseek(m_filedes, offset, whence);
            p[1]         = errno;
            m_communication->send(msg, send_len, 9);
            delete [] msg;
            break;
        }   // switch

    case Message::MSG_LSEEK64:
        {
            off64_t offset;
            int whence;
            Message_lseek_off_t m(buffer, len, &offset, &whence);
            int send_len = m.getSize(offset)+m.getSize(errno);
            char *msg    = new char[send_len];
            off_t *p     = (off_t*)msg;
            p[0]         = lseek(m_filedes, offset, whence);
            p[1]         = errno;
            m_communication->send(msg, send_len, 9);
            delete [] msg;
            break;
        }   // switch

    case Message::MSG_WRITE:
        {
            size_t nbyte;
            void *data;
            Message_write m(buffer, len, &nbyte, &data);

            write(m_filedes, data, nbyte);
            break;
        }
    case Message::MSG_READ:
        {
            size_t count;
            Message_read m(buffer, len, &count);

            ssize_t result;

            int send_len = m.getSize(result) + m.getSize(errno) + count;
            char *msg = new char[send_len];
            ssize_t *p = (ssize_t*)msg;
            p[0] = read(m_filedes, msg+ m.getSize(result) + m.getSize(errno), count);
            // We can't assign to p[1], since then 8 bytes would be written
            memcpy(p+1, &errno, sizeof(errno));
            m_communication->send(msg, send_len, 9);
            delete [] msg;
            break;
        }
    case Message::MSG_CLOSE:
        {
            Message_close m(buffer, len);
            int msg[2];
            msg[0] = close(m_filedes);
            msg[1] = errno;
            m_communication->send(msg, 2*sizeof(int), 9);
            break;
        }
    case Message::MSG_QUIT:
        {
            Message_quit m(buffer, len);
            return true;
        }
    default:
        {
            printf("Incorrect type %d found - ignored.\n",
                   (Message::MessageType)buffer[0]);
        }
    }   // switch
    return false;
}   // handleRequest

