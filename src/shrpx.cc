/*
 * Spdylay - SPDY Library
 *
 * Copyright (c) 2012 Tatsuhiro Tsujikawa
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#include "shrpx.h"

#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netdb.h>
#include <signal.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <getopt.h>
#include <syslog.h>

#include <limits>
#include <cstdlib>
#include <iostream>
#include <fstream>
#include <vector>

#include <openssl/ssl.h>
#include <openssl/err.h>

#include <event2/listener.h>

#include <spdylay/spdylay.h>

#include "shrpx_config.h"
#include "shrpx_listen_handler.h"
#include "shrpx_ssl.h"
#include "login_helper.h"

using namespace shrpx;

namespace {
void ssl_acceptcb(evconnlistener *listener, int fd,
                  sockaddr *addr, int addrlen, void *arg)
{
  ListenHandler *handler = reinterpret_cast<ListenHandler*>(arg);
  handler->accept_connection(fd, addr, addrlen);
}
} // namespace

namespace {
bool is_ipv6_numeric_addr(const char *host)
{
  uint8_t dst[16];
  return inet_pton(AF_INET6, host, dst) == 1;
}
} // namespace


namespace {
void evlistener_errorcb(evconnlistener *listener, void *ptr)
{
  LOG(ERROR) << "Accepting incoming connection failed";
}
} // namespace

namespace {
/**
 * 创建evconnlistener
 */
evconnlistener* create_evlistener(ListenHandler *handler, int family)
{
  // TODO Listen both IPv4 and IPv6
  addrinfo hints;
  int fd = -1;
  int r;
  
  //先做名称解析，把get_config()->host、get_config()->port解析成addrinfo
  char service[10];
  snprintf(service, sizeof(service), "%u", get_config()->port);
  memset(&hints, 0, sizeof(addrinfo));
  hints.ai_family = family;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;
#ifdef AI_ADDRCONFIG
  hints.ai_flags |= AI_ADDRCONFIG;
#endif // AI_ADDRCONFIG

  addrinfo *res, *rp;
  r = getaddrinfo(get_config()->host, service, &hints, &res);
  if(r != 0) {
    LOG(INFO) << "Unable to get IPv" << (family == AF_INET ? "4" : "6")
              << " address for " << get_config()->host << ": "
               << gai_strerror(r);
    return NULL;
  }
  //把getaddrinfo返回的addrinfo挨个尝试一遍
  for(rp = res; rp; rp = rp->ai_next) {
    fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
    if(fd == -1) {
      continue;
    }
    int val = 1;
    if(setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &val,
                  static_cast<socklen_t>(sizeof(val))) == -1) {
      close(fd);
      continue;
    }
    evutil_make_socket_nonblocking(fd);

    if(bind(fd, rp->ai_addr, rp->ai_addrlen) == 0) {
      break;
    }
    close(fd);
  }
  if(rp) {
    char host[NI_MAXHOST];
    r = getnameinfo(rp->ai_addr, rp->ai_addrlen, host, sizeof(host),
                        0, 0, NI_NUMERICHOST);
    if(r == 0) {
      LOG(INFO) << "Listening on " << host << ", port " << get_config()->port;
    } else {
      LOG(FATAL) << gai_strerror(r);
      DIE();
    }
  }
  freeaddrinfo(res);
  if(rp == 0) {
    if(ENABLE_LOG) {
      LOG(INFO) << "Listening " << (family == AF_INET ? "IPv4" : "IPv6")
                << " socket failed";
    }
    return 0;
  }

  evconnlistener *evlistener = evconnlistener_new
    (handler->get_evbase(),
     ssl_acceptcb,//listener callback
     handler,
     LEV_OPT_REUSEABLE | LEV_OPT_CLOSE_ON_FREE,
     get_config()->backlog,
     fd);
  evconnlistener_set_error_cb(evlistener, evlistener_errorcb);
  return evlistener;
}
} // namespace

namespace {
/** 切换到另一个用户 */
void drop_privileges()
{
  if(getuid() == 0 && get_config()->uid != 0) {
    if(setgid(get_config()->gid) != 0) {
      LOG(FATAL) << "Could not change gid: " << strerror(errno);
      exit(EXIT_FAILURE);
    }
    if(setuid(get_config()->uid) != 0) {
      LOG(FATAL) << "Could not change uid: " << strerror(errno);
      exit(EXIT_FAILURE);
    }
    if(setuid(0) != -1) {
      LOG(FATAL) << "Still have root privileges?";
      exit(EXIT_FAILURE);
    }
  }
}
} // namespace

namespace {
int event_loop()
{
  event_base *evbase = event_base_new();

  ListenHandler *listener_handler = new ListenHandler(evbase);

  if(get_config()->daemon) {
    if(daemon(0, 0) == -1) {
      LOG(FATAL) << "Failed to daemonize: " << strerror(errno);
      exit(EXIT_FAILURE);
    }
  }

  // ListenHandler loads private key. After that, we drop the root
  // privileges if needed.
  drop_privileges();

  evconnlistener *evlistener4;
  evlistener4 = create_evlistener(listener_handler, AF_INET);
  if(!evlistener4) {
    LOG(FATAL) << "Failed to listen on address "
               << get_config()->host << ", port " << get_config()->port;
    exit(EXIT_FAILURE);
  }


  if(ENABLE_LOG) {
    LOG(INFO) << "Entering event loop";
  }
  event_base_loop(evbase, 0);
  if(evlistener4) {
    evconnlistener_free(evlistener4);
  }

  return 0;
}
} // namespace

namespace {
void save_pid()
{
  std::ofstream out(get_config()->pid_file, std::ios::binary);
  out << getpid() << "\n";
  out.close();
  if(!out) {
    LOG(ERROR) << "Could not save PID to file " << get_config()->pid_file;
    exit(EXIT_FAILURE);
  }
}
} // namespace

namespace {
// Returns true if regular file or symbolic link |path| exists.
bool conf_exists(const char *path)
{
  struct stat buf;
  int rv = stat(path, &buf);
  return rv == 0 && (buf.st_mode & (S_IFREG | S_IFLNK));
}
} // namespace

namespace {
void fill_default_config()
{
  memset(mod_config(), 0, sizeof(*mod_config()));

  mod_config()->verbose = false;
  mod_config()->daemon = false;
  mod_config()->verify_client = false;

  mod_config()->server_name = "shrpx";
  set_config_str(&mod_config()->host, "0.0.0.0");
  mod_config()->port = 3000;
  mod_config()->private_key_file = 0;
  mod_config()->user_passwd_file = 0;
  mod_config()->cert_file = 0;

  // Read timeout for SPDY upstream connection
  mod_config()->spdy_upstream_read_timeout.tv_sec = 180;
  mod_config()->spdy_upstream_read_timeout.tv_usec = 0;

  // Read timeout for non-SPDY upstream connection
  mod_config()->upstream_read_timeout.tv_sec = 180;
  mod_config()->upstream_read_timeout.tv_usec = 0;

  // Write timeout for SPDY/non-SPDY upstream connection
  mod_config()->upstream_write_timeout.tv_sec = 60;
  mod_config()->upstream_write_timeout.tv_usec = 0;

  // Read/Write timeouts for downstream connection
  mod_config()->downstream_read_timeout.tv_sec = 900;
  mod_config()->downstream_read_timeout.tv_usec = 0;
  mod_config()->downstream_write_timeout.tv_sec = 60;
  mod_config()->downstream_write_timeout.tv_usec = 0;

  // Timeout for pooled (idle) connections
  mod_config()->downstream_idle_read_timeout.tv_sec = 60;

  // window bits for SPDY upstream/downstream connection. 2**16 =
  // 64KiB, which is SPDY/3 default.
  mod_config()->spdy_upstream_window_bits = 16;
  mod_config()->spdy_downstream_window_bits = 16;

  mod_config()->spdy_max_concurrent_streams =
    SPDYLAY_INITIAL_MAX_CONCURRENT_STREAMS;
  mod_config()->add_x_forwarded_for = false;
  mod_config()->no_via = false;
  mod_config()->accesslog = false;
  set_config_str(&mod_config()->conf_path, "/etc/shrpx/shrpx.conf");
  mod_config()->syslog = false;
  mod_config()->syslog_facility = LOG_DAEMON;
  mod_config()->use_syslog = false;
  // Default accept() backlog
  mod_config()->backlog = 256;
  mod_config()->ciphers = 0;
  mod_config()->client = false;
  mod_config()->insecure = false;
  mod_config()->pid_file = 0;
  mod_config()->uid = 0;
  mod_config()->gid = 0;  
  mod_config()->tty = isatty(fileno(stderr));
}
} // namespace

namespace {
void print_version(std::ostream& out)
{
  out << get_config()->server_name << std::endl;
}
} // namespace

namespace {
void print_usage(std::ostream& out)
{
  out << "Usage: shrpx [-D] [-c <config.ini>] [-L <LEVEL>]\n"
      << "\n"
      << "A reverse proxy for SPDY/HTTPS.\n"
      << std::endl;
}
} // namespace

namespace {
void print_help(std::ostream& out)
{
  print_usage(out);
}
} // namespace

int main(int argc, char **argv)
{
  Log::set_severity_level(WARNING);
  create_config();
  fill_default_config();
  std::vector<std::pair<const char*, const char*> > cmdcfgs;
  while(1) {   
    int c = getopt(argc, argv, "DL:c:vh");
    if(c == -1) {
      break;
    }
    switch(c) {
    case 'D':
      cmdcfgs.push_back(std::make_pair(SHRPX_OPT_DAEMON, "yes"));
      break;
    case 'L':
      cmdcfgs.push_back(std::make_pair(SHRPX_OPT_LOG_LEVEL, optarg));
      break;
    
    case 'c':
      set_config_str(&mod_config()->conf_path, optarg);
      break;    
    case 'h':
      print_help(std::cout);
      exit(EXIT_SUCCESS);
	  break;  
    case 'v':
      print_version(std::cout);
      exit(EXIT_SUCCESS);
	  break;
    case '?':
      exit(EXIT_FAILURE);   
	  break;  
    default:
      print_help(std::cout);
      exit(EXIT_SUCCESS);
    }
  }

  if(conf_exists(get_config()->conf_path)) {
    if(load_config(get_config()->conf_path) == -1) {
      LOG(FATAL) << "Failed to load configuration from "
                 << get_config()->conf_path;
      exit(EXIT_FAILURE);
    }
  }


  for(size_t i = 0, len = cmdcfgs.size(); i < len; ++i) {
    if(parse_config(cmdcfgs[i].first, cmdcfgs[i].second) == -1) {
      LOG(FATAL) << "Failed to parse command-line argument.";
      exit(EXIT_FAILURE);
    }
  }

  if(!get_config()->private_key_file || !get_config()->cert_file) {
    print_usage(std::cerr);
    LOG(FATAL) << "need ssl key";
    exit(EXIT_FAILURE);
  }

  if(get_config()->syslog) {
    openlog("shrpx", LOG_NDELAY | LOG_NOWAIT | LOG_PID,
            get_config()->syslog_facility);
    mod_config()->use_syslog = true;
  }

  if(get_config()->pid_file) {
    save_pid();
  }

  const char* passwd_file=get_config()->user_passwd_file;
  if(!passwd_file){
    LOG(FATAL) << "need provides passwd file";
    exit(EXIT_FAILURE);
  }
  initPasswd(passwd_file);

  struct sigaction act;
  memset(&act, 0, sizeof(struct sigaction));
  act.sa_handler = SIG_IGN;
  sigaction(SIGPIPE, &act, 0);

  OpenSSL_add_all_algorithms();
  SSL_load_error_strings();
  SSL_library_init();
  ssl::setup_ssl_lock();

  event_loop();

  ssl::teardown_ssl_lock();

  return 0;
}



