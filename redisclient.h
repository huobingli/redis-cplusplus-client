/* redisclient.h -- a C++ client library for redis.
 *
 * Copyright (c) 2009, Brian Hammond <brian at fictorial dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef REDISCLIENT_H
#define REDISCLIENT_H

#include <sys/errno.h>
#include <sys/socket.h>

#include <string>
#include <vector>
#include <map>
#include <set>
#include <stdexcept>
#include <ctime>
#include <sstream>

#include <boost/concept_check.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/functional/hash.hpp>
#include <boost/foreach.hpp>

#include <boost/random.hpp>

#include "anet.h"

#define REDIS_LBR                       "\r\n"
#define REDIS_STATUS_REPLY_OK           "OK"
#define REDIS_PREFIX_STATUS_REPLY_ERROR "-ERR "
#define REDIS_PREFIX_STATUS_REPLY_VALUE '+'
#define REDIS_PREFIX_SINGLE_BULK_REPLY  '$'
#define REDIS_PREFIX_MULTI_BULK_REPLY   '*'
#define REDIS_PREFIX_INT_REPLY          ':'
#define REDIS_WHITESPACE                " \f\n\r\t\v"
#include <boost/date_time/posix_time/ptime.hpp>
#include <boost/date_time/posix_time/posix_time_types.hpp>
#include <boost/optional.hpp>

namespace redis 
{
  template<typename CONSISTENT_HASHER>
  class base_client;
  
  struct connection_data
  {
    connection_data(const std::string & host = "localhost", uint16_t port = 6379, int dbindex = 0)
     : host(host), port(port), dbindex(dbindex), socket(ANET_ERR)
    {
    }
    
    std::string host;
    boost::uint16_t port;
    int dbindex;

  private:
    int socket;

    template<typename CONSISTENT_HASHER>
    friend class base_client;
  };
  
  enum server_role
  {
    role_master,
    role_slave
  };
  
#ifndef NDEBUG
  void output_proto_debug(const std::string & data, bool is_received = true)
  {
    std::string escaped_data(data);
    size_t pos;
    while ((pos = escaped_data.find("\n")) != std::string::npos)
      escaped_data.replace(pos, 1, "\\n");
    while ((pos = escaped_data.find("\r")) != std::string::npos)
      escaped_data.replace(pos, 1, "\\r");
    
    std::cerr << time(NULL) << ": "
              << (is_received ? "RECV '" : "SEND '")
              << escaped_data
              << "'"
              << std::endl;
  }
#endif
  
  std::vector<std::string>::size_type split(const std::string & str, char delim, std::vector<std::string> & elems)
  {
    std::stringstream ss(str);
    std::string item;
    std::vector<std::string>::size_type n = 0;
    while (getline(ss, item, delim))
    {
      elems.push_back(item);
      ++n;
    }
    return n;
  }
  
  inline std::string & rtrim(std::string & str, const std::string & ws = REDIS_WHITESPACE)
  {
    std::string::size_type pos = str.find_last_not_of(ws);
    str.erase(pos + 1);
    return str;
  }
  
  inline void split_lines(const std::string & str, std::vector<std::string> & elems)
  {
    split(str, '\n', elems);
    for (std::vector<std::string>::iterator it = elems.begin(); it != elems.end(); ++it)
      rtrim(*it);
  }
  
  // Generic error that is thrown when communicating with the redis server.
  
  class redis_error
  {
  public:
    redis_error(const std::string & err);
    operator std::string ();
    operator const std::string () const;
  private:
    std::string err_;
  };
  
  // Some socket-level I/O or general connection error.
  
  class connection_error : public redis_error
  {
  public:
    connection_error(const std::string & err);
  };
  
  // Redis gave us a reply we were not expecting.
  // Possibly an internal error (here or in redis, probably here).
  
  class protocol_error : public redis_error
  {
  public:
    protocol_error(const std::string & err);
  };
  
  // A key that you expected to exist does not in fact exist.
  
  class key_error : public redis_error
  {
  public:
    key_error(const std::string & err);
  };
  
  // A value of an expected type or other semantics was found to be invalid.
  
  class value_error : public redis_error
  {
  public:
    value_error(const std::string & err);
  };
  // Reads N bytes from given blocking socket.
  
  std::string read_n(int socket, ssize_t n)
  {
    char * buffer = new char[n + 1];
    buffer[n] = '\0';
    
    char * bp = buffer;
    ssize_t bytes_read = 0;
    
    while (bytes_read != n)
    {
      ssize_t bytes_received = 0;
      do
        bytes_received = recv(socket, bp, n - (bp - buffer), 0);
      while(bytes_received < 0 && errno == EINTR);
      
      if (bytes_received == 0)
        throw redis::connection_error("connection was closed");
      
      bytes_read += bytes_received;
      bp         += bytes_received;
    }
    
    std::string str(buffer, n);
    delete [] buffer;
    return str;
  }
  
  // Reads a single line of character data from the given blocking socket.
  // Returns the line that was read, not including EOL delimiter(s).  Both LF
  // ('\n') and CRLF ("\r\n") delimiters are supported.  If there was an I/O
  // error reading from the socket, connection_error is raised.  If max_size
  // bytes are read before finding an EOL delimiter, a blank string is
  // returned.
  
  std::string read_line(int socket, ssize_t max_size = 2048)
  {
    assert(socket > 0);
    assert(max_size > 0);
    
    std::ostringstream oss;
    
    enum { buffer_size = 64 };
    char buffer[buffer_size];
    memset(buffer, 0, buffer_size);
    
    ssize_t total_bytes_read = 0;
    bool found_delimiter = false;
    
    while (total_bytes_read < max_size && !found_delimiter)
    {
      // Peek at what's available.
      
      ssize_t bytes_received = 0;
      do
        bytes_received = recv(socket, buffer, buffer_size, MSG_PEEK);
      while(bytes_received < 0 && errno == EINTR);
      
      if (bytes_received == 0)
        throw connection_error("connection was closed");
      
      // Some data is available; Length might be < buffer_size.
      // Look for newline in whatever was read though.
      
      char * eol = static_cast<char *>(memchr(buffer, '\n', bytes_received));
      
      // If found, write data from the buffer to the output string.
      // Else, write the entire buffer and continue reading more data.
      
      ssize_t to_read = bytes_received;
      
      if (eol)
      {
        to_read = eol - buffer + 1;
        oss.write(buffer, to_read);
        found_delimiter = true;
      }
      else
        oss.write(buffer, bytes_received);
      
      // Now read from the socket to remove the peeked data from the socket's
        // read buffer.  This will not block since we've peeked already and know
        // there's data waiting.  It might fail if we were interrupted however.
        
        do
          bytes_received = recv(socket, buffer, to_read, 0);
        while(bytes_received < 0 && errno == EINTR);
    }
    
    // Construct final line string. Remove trailing CRLF-based whitespace.
    
    std::string line = oss.str();
    return rtrim(line, REDIS_LBR);
  }
  
  class makecmd
  {
  public:
    explicit makecmd(const std::string & cmd_name)
    {
      append(cmd_name);
      //if (!finalize)
      //  buffer_ << " ";
    }
    
    inline makecmd & operator<<(const std::string & datum)
    {
      append(datum);
      return *this;
    }
    
    template <typename T>
    makecmd & operator<<(T const & datum)
    {
      append( boost::lexical_cast<std::string>(datum) );
      return *this;
    }
    
    makecmd & operator<<(const std::vector<std::string> & data)
    {
      lines_.insert( lines_.end(), data.begin(), data.end() );
      return *this;
    }
    
    template <typename T>
    makecmd & operator<<(const std::vector<T> & data)
    {
      size_t n = data.size();
      for (size_t i = 0; i < n; ++i)
      {
        append( boost::lexical_cast<std::string>( data[i] ) );
        //if (i < n - 1)
        //  buffer_ << " ";
      }
      return *this;
    }
    
    operator std::string () const
    {
      std::ostringstream oss;
      size_t n = lines_.size();
      oss << REDIS_PREFIX_MULTI_BULK_REPLY << n << REDIS_LBR;
      
      for (size_t i = 0; i < n; ++i)
      {
        const std::string & param = lines_[i];
        oss << REDIS_PREFIX_SINGLE_BULK_REPLY << param.size() << REDIS_LBR;
        oss << param << REDIS_LBR;
      }
      
      return oss.str();
    }
    
  private:
    void append(const std::string & param)
    {
      lines_.push_back(param);
    }
    
    std::vector<std::string> lines_;
  };
  

  struct server_info 
  {
    std::string version;
    bool bgsave_in_progress;
    unsigned long connected_clients;
    unsigned long connected_slaves;
    unsigned long used_memory;
    unsigned long changes_since_last_save;
    unsigned long last_save_time;
    unsigned long total_connections_received;
    unsigned long total_commands_processed;
    unsigned long uptime_in_seconds;
    unsigned long uptime_in_days;
    server_role role;
    unsigned short arch_bits;
    std::string multiplexing_api;
    std::map<std::string, std::string> param_map;
  };

  // You should construct a 'client' object per connection to a redis-server.
  //
  // Please read the online redis command reference:
  // http://code.google.com/p/redis/wiki/CommandReference
  //
  // No provisions for customizing the allocator on the string/bulk value type
  // (std::string) are provided.  If needed, you can always change the
  // string_type typedef in your local version.

  template<typename CONSISTENT_HASHER>
  class base_client
  {
  private:
    void init(connection_data & con)
    {
      char err[ANET_ERR_LEN];
      con.socket = anetTcpConnect(err, const_cast<char*>(con.host.c_str()), con.port);
      if (con.socket == ANET_ERR)
        throw connection_error(err);
      anetTcpNoDelay(NULL, con.socket);
      select(con.dbindex, con);
    }
    
  public:
    typedef std::string string_type;
    typedef std::vector<string_type> string_vector;
    typedef std::pair<string_type, string_type> string_pair;
    typedef std::vector<string_pair> string_pair_vector;
    typedef std::set<string_type> string_set;

    typedef long int_type;

    explicit base_client(const string_type & host = "localhost",
                    uint16_t port = 6379, int_type dbindex = 0)
    {
      connection_data con;
      con.host = host;
      con.port = port;
      con.dbindex = dbindex;
      init(con);
      connections_.push_back(con);
    }

    template<typename CON_ITERATOR>
    base_client(CON_ITERATOR begin, CON_ITERATOR end)
    {
      while(begin != end)
      {
        connection_data con = *begin;
        init(con);
        connections_.push_back(con);
        begin++;
      }

      if( connections_.empty() )
        throw std::runtime_error("No connections given!");
    }

    inline static string_type missing_value()
    {
      return "**nonexistent-key**";
    }

    enum datatype 
    {
      datatype_none,      // key doesn't exist
      datatype_string,
      datatype_list,
      datatype_set,
      datatype_zset,
      datatype_hash,
      datatype_unknown
    };

    int_type get_list(const string_type & key, string_vector & out)
    {
      return lrange(key, 0, -1, out);
    }

    void lrem_exact(const string_type & key,
                    int_type count,
                    const string_type & value)
    { 
      if (lrem(key, count, value) != count)
        throw value_error("failed to remove exactly N elements from list");
    }
    enum range_specifier
    {
      exclude_min = 1 << 0,
      exclude_max = 1 << 1
    };

    enum aggregate_type
    {
      sum = 1 << 0,
      min = 1 << 1,
      max = 1 << 2
    };

    enum sort_order
    {
      sort_order_ascending,
      sort_order_descending
    };
    
    ~base_client()
    {
      BOOST_FOREACH(connection_data & con, connections_)
      {
        if (con.socket != ANET_ERR)
          close(con.socket);
      }
    }

    const std::vector<connection_data> & connections() const
    {
      return connections_;
    }
    
    void auth(const string_type & pass)
    {
      if( connections_.size() > 1 )
        throw std::runtime_error("feature is not available in cluster mode");

      int socket = connections_[0].socket;
      send_(socket, makecmd("AUTH") << pass);
      recv_ok_reply_(socket);
    }
    
    void set(const string_type & key,
                          const string_type & value)
    {
      int socket = get_socket(key);
      send_(socket, makecmd("SET") << key << value);
      recv_ok_reply_(socket);
    }
    
    void mset( const string_vector & keys, const string_vector & values )
    {
      assert( keys.size() == values.size() );

      std::map< int, boost::optional<makecmd> > socket_commands;

      for(size_t i=0; i < keys.size(); i++)
      {
        int socket = get_socket(keys);
        boost::optional<makecmd> & cmd = socket_commands[socket];
        if(!cmd)
          cmd = makecmd("MSET");
        *cmd << keys[i] << values[i];
      }

      typedef std::pair< int, boost::optional<makecmd> > sock_pair;
      BOOST_FOREACH(const sock_pair & sp, socket_commands)
      {
        send_(sp.first, *sp.second);
      }
      
      BOOST_FOREACH(const sock_pair & sp, socket_commands)
      {
        recv_ok_reply_(sp.first);
      }
    }
    
    void mset( const string_pair_vector & key_value_pairs )
    {
      std::map< int, boost::optional<makecmd> > socket_commands;
      
      for(size_t i=0; i < key_value_pairs.size(); i++)
      {
        const string_type & key = key_value_pairs[i].first;
        const string_type & value = key_value_pairs[i].second;
        
        int socket = get_socket(key);
        boost::optional<makecmd> & cmd = socket_commands[socket];
        if(!cmd)
          cmd = makecmd("MSET");
        *cmd << key << value;
      }
      
      typedef std::pair< int, boost::optional<makecmd> > sock_pair;
      BOOST_FOREACH(const sock_pair & sp, socket_commands)
      {
        send_(sp.first, *sp.second);
      }

      BOOST_FOREACH(const sock_pair & sp, socket_commands)
      {
        recv_ok_reply_(sp.first);
      }
    }

  private:
    struct msetex_data
    {
      boost::optional<makecmd> mset_cmd;
      std::string expire_cmds;
      size_t count;
    };

  public:
    void msetex( const string_pair_vector & key_value_pairs, int_type seconds )
    {
      std::map< int, msetex_data > socket_commands;
      
      for(size_t i=0; i < key_value_pairs.size(); i++)
      {
        const string_type & key = key_value_pairs[i].first;
        const string_type & value = key_value_pairs[i].second;
        
        int socket = get_socket(key);
        msetex_data & dat = socket_commands[socket];
        boost::optional<makecmd> & cmd = dat.mset_cmd;
        if(!cmd)
        {
          cmd = makecmd("MSET");
          dat.count = 0;
        }
        *cmd << key << value;

        std::string & expire_cmds = dat.expire_cmds;
        expire_cmds += makecmd("EXPIRE") << key << seconds;
        dat.count++;
      }
      
      typedef std::pair< int, msetex_data > sock_pair;
      BOOST_FOREACH(const sock_pair & sp, socket_commands)
      {
        std::string cmds = *sp.second.mset_cmd;
        cmds += sp.second.expire_cmds;
        send_(sp.first, cmds);
      }
      
      BOOST_FOREACH(const sock_pair & sp, socket_commands)
      {
        recv_ok_reply_(sp.first);
        for(size_t i= 0; i < sp.second.count; i++)
          recv_int_ok_reply_(sp.first);
        
      }
    }
    
    string_type get(const string_type & key)
    {
      int socket = get_socket(key);
      send_(socket, makecmd("GET") << key);
      return recv_bulk_reply_(socket);
    }
    
    string_type getset(const string_type & key, const string_type & value)
    {
      int socket = get_socket(key);
      send_(socket, makecmd("GETSET") << key << value);
      return recv_bulk_reply_(socket);
    }

  private:
    struct connection_keys
    {
      boost::optional<makecmd> cmd;

      /// Gives the position of the value in the original array
      std::vector<size_t> indices;
    };

  public:
    void mget(const string_vector & keys, string_vector & out)
    {
      out = string_vector( keys.size() );
      std::map< int, connection_keys > socket_commands;
      
      for(size_t i=0; i < keys.size(); i++)
      {
        int socket = get_socket(keys[i]);
        connection_keys & con_keys = socket_commands[socket];
        boost::optional<makecmd> & cmd = con_keys.cmd;
        if(!cmd)
          cmd = makecmd("MGET");
        *cmd << keys[i];
        con_keys.indices.push_back(i);
      }
      
      typedef std::pair< int, connection_keys > sock_pair;
      BOOST_FOREACH(const sock_pair & sp, socket_commands)
      {
        send_(sp.first, *sp.second.cmd);
      }
      
      BOOST_FOREACH(const sock_pair & sp, socket_commands)
      {
        const connection_keys & con_keys = sp.second;
        string_vector cur_out;
        recv_multi_bulk_reply_(sp.first, cur_out);
        
        for(size_t i=0; i < cur_out.size(); i++)
          out[con_keys.indices[i]] = cur_out[i];
      }
    }
    
    bool setnx(const string_type & key,
                            const string_type & value)
    {
      int socket = get_socket(key);
      send_(socket, makecmd("SETNX") << key << value);
      return recv_int_reply_(socket) == 1;
    }
    
    bool msetnx( const string_vector & keys, const string_vector & values )
    {
      assert( keys.size() == values.size() );
      
      std::map< int, boost::optional<makecmd> > socket_commands;
      
      for(size_t i=0; i < keys.size(); i++)
      {
        int socket = get_socket(keys);
        boost::optional<makecmd> & cmd = socket_commands[socket];
        if(!cmd)
          cmd = makecmd("MSETNX");
        *cmd << keys[i] << values[i];
      }

      if( socket_commands.size() > 1 )
        throw std::runtime_error("feature is not available in cluster mode");
      
      typedef std::pair< int, boost::optional<makecmd> > sock_pair;
      BOOST_FOREACH(const sock_pair & sp, socket_commands)
      {
        send_(sp.first, *sp.second);
        recv_ok_reply_(sp.first);
      }
    }
    
    bool msetnx( const string_pair_vector & key_value_pairs )
    {
      std::map< int, boost::optional<makecmd> > socket_commands;
      
      for(size_t i=0; i < key_value_pairs.size(); i++)
      {
        int socket = get_socket(keys);
        boost::optional<makecmd> & cmd = socket_commands[socket];
        if(!cmd)
          cmd = makecmd("MSETNX");
        *cmd << key_value_pairs[i].first << key_value_pairs[i].second;
      }
      
      if( socket_commands.size() > 1 )
        throw std::runtime_error("feature is not available in cluster mode");
      
      typedef std::pair< int, boost::optional<makecmd> > sock_pair;
      BOOST_FOREACH(const sock_pair & sp, socket_commands)
      {
        send_(sp.first, *sp.second);
        recv_ok_reply_(sp.first);
      }
    }
    
    void setex(const string_type & key, const string_type & value, unsigned int secs)
    {
      int socket = get_socket(key);
      send_(socket, makecmd("SETEX") << key << secs << value);
      recv_ok_reply_(socket);
    }
    
    size_t append(const string_type & key, const string_type & value)
    {
      int socket = get_socket(key);
      send_(socket, makecmd("APPEND") << key << value);
      int res = recv_int_reply_(socket);
      if(res < 0)
        throw protocol_error("expected value size");
      
      assert( static_cast<size_t>(res) >= value.size() );
      return static_cast<size_t>(res);
    }
    
    string_type substr(const string_type & key, int start, int end)
    {
      int socket = get_socket(key);
      send_(socket, makecmd("SUBSTR") << key << start << end);
      return recv_bulk_reply_(socket);
    }
    
    int_type incr(const string_type & key)
    {
      int socket = get_socket(key);
      send_(socket, makecmd("INCR") << key);
      return recv_int_reply_(socket);
    }
    
    int_type incrby(const string_type & key,
                                              int_type by)
    {
      int socket = get_socket(key);
      send_(socket, makecmd("INCRBY") << key << by);
      return recv_int_reply_(socket);
    }
    
    int_type decr(const string_type & key)
    {
      int socket = get_socket(key);
      send_(socket, makecmd("DECR") << key);
      return recv_int_reply_(socket);
    }
    
    int_type decrby(const string_type & key,
                                              int_type by)
    {
      int socket = get_socket(key);
      send_(socket, makecmd("DECRBY") << key << by);
      return recv_int_reply_(socket);
    }
    
    bool exists(const string_type & key)
    {
      int socket = get_socket(key);
      send_(socket, makecmd("EXISTS") << key);
      return recv_int_reply_(socket) == 1;
    }
    
    void del(const string_type & key)
    {
      int socket = get_socket(key);
      send_(socket, makecmd("DEL") << key);
      recv_int_ok_reply_(socket);
    }
    
    datatype type(const string_type & key)
    {
      int socket = get_socket(key);
      send_(socket, makecmd("TYPE") << key);
      std::string response = recv_single_line_reply_(socket);
      
      if(response == "none")   return datatype_none;
      if(response == "string") return datatype_string;
      if(response == "list")   return datatype_list;
      if(response == "set")    return datatype_set;
      if(response == "zset")   return datatype_zset;
      if(response == "hash")   return datatype_hash;

#ifndef NDEBUG
      std::cerr << "Got unknown datatype name: " << response << std::endl;
#endif // NDEBUG
      
      return datatype_unknown;
    }
    
    int_type keys(const string_type & pattern, string_vector & out)
    {
      BOOST_FOREACH(const connection_data & con, connections_)
      {
        send_(con.socket, makecmd("KEYS") << pattern);
      }

      int_type res = 0;
      
      BOOST_FOREACH(const connection_data & con, connections_)
      {
        res += recv_multi_bulk_reply_(con.socket, out);
      }
      
      return res;
    }
    
    string_type randomkey()
    {      
      int socket = connections_[0].socket;
      if( connections_.size() > 1 )
      {
        /*
         * Select a random server if there are more then one
         */
        
        boost::mt19937 gen;
        boost::posix_time::ptime now = boost::posix_time::microsec_clock::local_time();
        boost::posix_time::ptime epoch( boost::gregorian::date(1970, 1, 1) );
        gen.seed( (now-epoch).total_seconds() );
        
        boost::uniform_int<> dist(0, connections_.size());
        boost::variate_generator< boost::mt19937&, boost::uniform_int<> > die(gen, dist);
        socket = connections_[die()].socket;
      }
      
      send_(socket, makecmd("RANDOMKEY") );
      return recv_bulk_reply_(socket);
    }
    
    /**
     * @warning Not cluster save (the old name and the new one must be on the same redis server)
     */
    void rename(const string_type & old_name, const string_type & new_name)
    {
      int source_socket = get_socket(old_name);
      int destin_socket = get_socket(new_name);
      if( source_socket != destin_socket )
        throw std::runtime_error("feature is not available in cluster mode");

      send_(source_socket, makecmd("RENAME") << old_name << new_name);
      recv_ok_reply_(source_socket);
    }
    
    /**
     * @warning Not cluster save (the old name and the new one must be on the same redis server)
     */
    bool renamenx(const string_type & old_name, const string_type & new_name)
    {
      int source_socket = get_socket(old_name);
      int destin_socket = get_socket(new_name);
      
      if( source_socket != destin_socket )
        throw std::runtime_error("feature is not available in cluster mode");
      
      send_(source_socket, makecmd("RENAMENX") << old_name << new_name);
      return recv_int_reply_(source_socket) == 1;
    }

    /**
     * @returns the number of keys in the currently selected database. In cluster mode the number
     * of keys in all currently selected databases is returned.
     */
    int_type dbsize()
    {
      int_type val = 0;
      
      BOOST_FOREACH(const connection_data & con, connections_)
      {
        send_(con.socket, makecmd("DBSIZE"));
      }
      
      BOOST_FOREACH(const connection_data & con, connections_)
      {
        val += recv_int_reply_(con.socket);
      }
      
      return val;
    }

    /**
     * @returns the number of keys in the currently selected database with the given connection.
     */
    int_type dbsize(const connection_data & con)
    {
      send_(con.socket, makecmd("DBSIZE"));
      return recv_int_reply_(con.socket);
    }
    
    void expire(const string_type & key, unsigned int secs)
    {
      int socket = get_socket(key);
      send_(socket, makecmd("EXPIRE") << key << secs);
      recv_int_ok_reply_(socket);
    }
    
    int ttl(const string_type & key)
    {
      int socket = get_socket(key);
      send_(socket, makecmd("TTL") << key);
      return recv_int_reply_(socket);
    }
    
    int_type rpush(const string_type & key,
                            const string_type & value)
    {
      int socket = get_socket(key);
      send_(socket, makecmd("RPUSH") << key << value);
      return recv_int_reply_(socket);
    }
    
    int_type lpush(const string_type & key,
                            const string_type & value)
    {
      int socket = get_socket(key);
      send_(socket, makecmd("LPUSH") << key << value);
      return recv_int_reply_(socket);
    }
    
    int_type llen(const string_type & key)
    {
      int socket = get_socket(key);
      send_(socket, makecmd("LLEN") << key);
      return recv_int_reply_(socket);
    }
    
    int_type lrange(const string_type & key,
                    int_type start,
                    int_type end,
                    string_vector & out)
    {
      int socket = get_socket(key);
      send_(socket, makecmd("LRANGE") << key << start << end);
      return recv_multi_bulk_reply_(socket, out);
    }
    
    void ltrim(const string_type & key,
                            int_type start,
                            int_type end)
    {
      int socket = get_socket(key);
      send_(socket, makecmd("LTRIM") << key << start << end);
      recv_ok_reply_(socket);
    }
    
    string_type lindex(const string_type & key,
                                                 int_type index)
    {
      int socket = get_socket(key);
      send_(socket, makecmd("LINDEX") << key << index);
      return recv_bulk_reply_(socket);
    }
    
    void lset(const string_type & key, int_type index, const string_type & value)
    {
      int socket = get_socket(key);
      send_(socket, makecmd("LSET") << key << index << value);
      recv_ok_reply_(socket);
    }
    
    int_type lrem(const string_type & key, int_type count, const string_type & value)
    {
      int socket = get_socket(key);
      send_(socket, makecmd("LREM") << key << count << value);
      return recv_int_reply_(socket);
    }
    
    string_type lpop(const string_type & key)
    {
      int socket = get_socket(key);
      send_(socket, makecmd("LPOP") << key);
      return recv_bulk_reply_(socket);
    }
    
    string_type rpop(const string_type & key)
    {
      int socket = get_socket(key);
      send_(socket, makecmd("RPOP") << key);
      return recv_bulk_reply_(socket);
    }

    /**
     * @warning Not cluster save (all keys must be on the same redis server)
     */
    string_pair blpop(const string_vector & keys, int_type timeout)
    {
      int socket = get_socket(keys);
      makecmd m("BLPOP");
      for(size_t i=0; i < keys.size(); i++)
        m << keys[i];
      m << timeout;
      send_(socket, m);
      string_vector sv;
      recv_multi_bulk_reply_(socket, sv);
      if(sv.size() == 2)
        return make_pair( sv[0], sv[1] );
      else
        return make_pair( "", missing_value() );
    }
    
    string_type blpop(const string_type & key, int_type timeout)
    {
      int socket = get_socket(key);
      send_(socket, makecmd("BLPOP") << key << timeout);
      string_vector sv;
      recv_multi_bulk_reply_(socket, sv);
      if(sv.size() == 2)
      {
        assert(key == sv[0]);
        return sv[1];
      }
      else
        return missing_value();
    }
    
    /**
     * @warning Not cluster save (all keys must be on the same redis server)
     */
    string_pair brpop(const string_vector & keys, int_type timeout)
    {
      int socket = get_socket(keys);
      makecmd m("BRPOP");
      for(size_t i=0; i < keys.size(); i++)
        m << keys[i];
      m << timeout;
      send_(socket, m);
      string_vector sv;
      recv_multi_bulk_reply_(socket, sv);
      if(sv.size() == 2)
        return make_pair( sv[0], sv[1] );
      else
        return make_pair( "", missing_value );
    }
    
    string_type brpop(const string_type & key, int_type timeout)
    {
      int socket = get_socket(key);
      send_(socket, makecmd("BRPOP") << key << timeout);
      string_vector sv;
      recv_multi_bulk_reply_(socket, sv);
      if(sv.size() == 2)
      {
        assert(key == sv[0]);
        return sv[1];
      }
      else
        return missing_value();
    }
    
    void sadd(const string_type & key,
                           const string_type & value)
    {
      int socket = get_socket(key);
      send_(socket, makecmd("SADD") << key << value);
      recv_int_ok_reply_(socket);
    }
    
    void srem(const string_type & key,
                           const string_type & value)
    {
      int socket = get_socket(key);
      send_(socket, makecmd("SREM") << key << value);
      recv_int_ok_reply_(socket);
    }
    
    string_type spop(const string_type & key)
    {
      int socket = get_socket(key);
      send_(socket, makecmd("SPOP") << key);
      return recv_bulk_reply_(socket);
    }
    
    void smove(const string_type & srckey, const string_type & dstkey, const string_type & member)
    {
      int src_socket = get_socket(srckey);
      int dst_socket = get_socket(dstkey);
      if(dst_socket != src_socket)
        throw std::runtime_error("function not available in cluster mode");
        
      send_(src_socket, makecmd("SMOVE") << srckey << dstkey << member);
      recv_int_ok_reply_(src_socket);
    }
    
    int_type scard(const string_type & key)
    {
      int socket = get_socket(key);
      send_(socket, makecmd("SCARD") << key);
      return recv_int_reply_(socket);
    }
    
    bool sismember(const string_type & key,
                                const string_type & value)
    {
      int socket = get_socket(key);
      send_(socket, makecmd("SISMEMBER") << key << value);
      return recv_int_reply_(socket) == 1;
    }

    /**
     * @returns the intersection between the Sets stored at key1, key2, ..., keyN
     * @warning Not cluster save (all keys must be on the same redis server)
     */
    int_type sinter(const string_vector & keys, string_set & out)
    {
      int socket = get_socket(keys);
      send_(socket, makecmd("SINTER") << keys);
      return recv_multi_bulk_reply_(socket, out);
    }

    /**
     * @warning Not cluster save (all keys must be on the same redis server)
     */
    int_type sinterstore(const string_type & dstkey, const string_vector & keys)
    {
      int socket = get_socket(dstkey);
      int source_sockets = get_socket(keys);
      if(socket != source_sockets)
        throw std::runtime_error("not available in cluster mode");
      
      send_(socket, makecmd("SINTERSTORE") << dstkey << keys);
      return recv_int_reply_(socket);
    }
    
    int_type sunion(const string_vector & keys, string_set & out)
    {
      int socket = get_socket(keys);
      send_(socket, makecmd("SUNION") << keys);
      return recv_multi_bulk_reply_(socket, out);
    }
    
    int_type sunionstore(const string_type & dstkey,
                                                   const string_vector & keys)
    {
      int socket = get_socket(dstkey);
      int source_sockets = get_socket(keys);
      if(socket != source_sockets)
        throw std::runtime_error("not available in cluster mode");
      
      send_(socket, makecmd("SUNIONSTORE") << dstkey << keys);
      return recv_int_reply_(socket);
    }
    
    int_type sdiff(const string_vector & keys, string_set & out)
    {
      int socket = get_socket(keys);
      send_(socket, makecmd("SDIFF") << keys);
      return recv_multi_bulk_reply_(socket, out);
    }
    
    int_type sdiffstore(const string_type & dstkey, const string_vector & keys)
    {
      int socket = get_socket(dstkey);
      int source_sockets = get_socket(keys);
      if(socket != source_sockets)
        throw std::runtime_error("not available in cluster mode");
      
      send_(socket, makecmd("SDIFFSTORE") << dstkey << keys);
      return recv_int_reply_(socket);
    }
    
    int_type smembers(const string_type & key, string_set & out)
    {
      int socket = get_socket(key);
      send_(socket, makecmd("SMEMBERS") << key);
      return recv_multi_bulk_reply_(socket, out);
    }
    
    string_type srandmember(const string_type & key)
    {
      int socket = get_socket(key);
      send_(socket, makecmd("SPOP") << key);
      return recv_bulk_reply_(socket);
    }
    
    void zadd(const string_type & key, double score, const string_type & value)
    {
      int socket = get_socket(key);
      send_(socket, makecmd("ZADD") << key << score << value);
      recv_int_ok_reply_(socket);
    }
    
    void zrem(const string_type & key, const string_type & value)
    {
      int socket = get_socket(key);
      send_(socket, makecmd("ZREM") << key << value);
      recv_int_ok_reply_(socket);
    }
    
    double zincrby(const string_type & key, const string_type & value, double increment)
    {
      int socket = get_socket(key);
      send_(socket, makecmd("ZINCRBY") << key << value << increment);
      return boost::lexical_cast<double>( recv_bulk_reply_(socket) );
    }
    
    int_type zrank(const string_type & key, const string_type & value)
    {
      int socket = get_socket(key);
      send_(socket, makecmd("ZRANK") << key << value);
      return recv_int_reply_(socket);
    }
    
    int_type zrevrank(const string_type & key, const string_type & value)
    {
      int socket = get_socket(key);
      send_(socket, makecmd("ZREVRANK") << key << value);
      return recv_int_reply_(socket);
    }
    
    void zrangebyscore(const string_type & key, double min, double max, string_vector & out, int_type offset, int_type max_count, int range_modification)
    {
      int socket = get_socket(key);
      std::string min_str, max_str;
      if( range_modification & exclude_min )
        min_str = "(";
      if( range_modification & exclude_max )
        max_str = "(";
      
      min_str += boost::lexical_cast<std::string>(min);
      max_str += boost::lexical_cast<std::string>(max);
      
      makecmd m("ZRANGEBYSCORE");
      m << key << min_str << max_str;
        
      if(max_count > 0 || offset > 0)
        m << "LIMIT" << offset << max_count;
        
      send_(socket, m);
      recv_multi_bulk_reply_(socket, out);
    }
    
    int_type zcount(const string_type & key)
    {
      int socket = get_socket(key);
      send_(socket, makecmd("ZCOUNT") << key);
      return recv_int_reply_(socket);
    }
    
    int_type zremrangebyrank( const string_type & key, int_type start, int_type end )
    {
      int socket = get_socket(key);
      send_(socket, makecmd("ZREMRANGEBYRANK") << key << start << end);
      return recv_int_reply_(socket);
    }
    
    int_type zremrangebyscore( const string_type& key, double min, double max )
    {
      int socket = get_socket(key);
      send_(socket, makecmd("ZREMRANGEBYSCORE") << key << min << max);
      return recv_int_reply_(socket);
    }
    
    int_type zcard( const string_type & key )
    {
      int socket = get_socket(key);
      send_(socket, makecmd("ZCARD") << key);
      return recv_int_reply_(socket);
    }
    
    double zscard( const string_type& key, const string_type& element )
    {
      int socket = get_socket(key);
      send_(socket, makecmd("ZSCARD") << key << element);
      return boost::lexical_cast<double>( recv_bulk_reply_(socket) );
    }
    
    int_type zunionstore( const string_type & dstkey, const string_vector & keys, const string_vector & weights, aggregate_type aggragate )
    {
      int dst_socket = get_socket(dstkey);
      int socket = get_socket(keys);
      if(socket != dst_socket)
        throw std::runtime_error("feature is not available in cluster mode");
      
      makecmd m("ZUNIONSTORE");
      m << dstkey << keys.size() << keys;
      if( weights.size() > 0 )
      {
        assert(keys.size() == weights.size());
        m << weights.size() << weights;
      }
      m << "AGGREGATE";
      switch(aggragate)
      {
        case sum:
          m << "SUM";
        case min:
          m << "MIN";
        case max:
          m << "MAX";
      }
      send_(socket, m);
      return recv_int_reply_(socket);
    }
    
    int_type zinterstore(const string_type & dstkey, const string_vector & keys, const string_vector & weights, aggregate_type aggragate )
    {
      int dst_socket = get_socket(dstkey);
      int socket = get_socket(keys);
      if(socket != dst_socket)
        throw std::runtime_error("feature is not available in cluster mode");
      
      makecmd m("ZINTERSTORE");
      m << dstkey << keys.size() << keys;
      if( weights.size() > 0 )
      {
        assert(keys.size() == weights.size());
        m << weights.size() << weights;
      }
      m << "AGGREGATE";
      switch(aggragate)
      {
        case sum:
          m << "SUM";
        case min:
          m << "MIN";
        case max:
          m << "MAX";
      }
      send_(socket, m);
      return recv_int_reply_(socket);
    }
    
    bool hset( const string_type & key, const string_type & field, const string_type & value )
    {
      int socket = get_socket(key);
      send_(socket, makecmd("HSET") << key << field << value);
      return recv_int_reply_(socket) == 1;
    }
    
    string_type hget( const string_type & key, const string_type & field )
    {
      int socket = get_socket(key);
      send_(socket, makecmd("HGET") << key << field);
      return recv_bulk_reply_(socket);
    }
    
    bool hsetnx( const string_type & key, const string_type & field, const string_type & value )
    {
      int socket = get_socket(key);
      send_(socket, makecmd("HSETNX") << key << field << value);
      return recv_int_reply_(socket) == 1;
    }
    
    void hmset( const string_type & key, const string_vector & fields, const string_vector& values )
    {
      int socket = get_socket(key);
      makecmd m("HMSET");
      m << key;
      assert( fields.size() == values.size() );
      
      for(size_t i=0; i < fields.size(); i++)
        m << fields[i] << values[i];
      
      send_(socket, m);
      recv_ok_reply_(socket);
    }
    
    void hmset( const string_type & key, const string_pair_vector & field_value_pairs )
    {
      int socket = get_socket(key);
      makecmd m("HMSET");
      m << key;
      
      for(size_t i=0; i < field_value_pairs.size(); i++)
        m << field_value_pairs[i].first << field_value_pairs[i].second;
      
      send_(socket, m);
      recv_ok_reply_(socket);
    }
    
    void hmget( const string_type & key, const string_vector & fields, string_vector & out)
    {
      int socket = get_socket(key);
      makecmd m("HMGET");
      m << key;
      
      for(size_t i=0; i < fields.size(); i++)
        m << fields[i];

      send_(socket, m);
      recv_multi_bulk_reply_(out);
    }
    
    int_type hincrby( const string_type & key, const string_type & field, int_type by )
    {
      int socket = get_socket(key);
      send_(socket, makecmd("HINCRBY") << key << field << by);
      return recv_int_reply_(socket);
    }
    
    bool hexists( const string_type & key, const string_type & field )
    {
      int socket = get_socket(key);
      send_(socket, makecmd("HEXISTS") << key << field);
      return recv_int_reply_(socket) == 1;
    }
    
    bool hdel( const string_type& key, const string_type& field )
    {
      int socket = get_socket(key);
      send_(socket, makecmd("HDEL") << key << field);
      return recv_int_reply_(socket) == 1;
    }
    
    int_type hlen( const string_type & key )
    {
      int socket = get_socket(key);
      send_(socket, makecmd("HLEN") << key);
      return recv_int_reply_(socket);
    }
    
    void hkeys( const string_type & key, string_vector & out )
    {
      int socket = get_socket(key);
      send_(socket, makecmd("HKEYS") << key);
      recv_multi_bulk_reply_(socket, out);
    }
    
    void hvals( const string_type & key, string_vector & out )
    {
      int socket = get_socket(key);
      send_(socket, makecmd("HVALS") << key);
      recv_multi_bulk_reply_(socket, out);
    }
    
    void hgetall( const string_type & key, string_pair_vector & out )
    {
      int socket = get_socket(key);
      send_(socket, makecmd("HGETALL") << key);
      string_vector s;
      recv_multi_bulk_reply_(socket, s);
      for(size_t i = 0; i < s.size(); i+=2)
        out.push_back( make_pair(s[i], s[i+1]) );
    }
    
    void select(int_type dbindex)
    {
      if( connections_.size() > 1 )
        throw std::runtime_error("feature is not available in cluster mode");
      
      int socket = connections_[0].socket;
      send_(socket, makecmd("SELECT") << dbindex);
      recv_ok_reply_(socket);
    }
    
    void select(int_type dbindex, const connection_data & con)
    {
      int socket = con.socket;
      send_(socket, makecmd("SELECT") << dbindex);
      recv_ok_reply_(socket);
    }
    
    void move(const string_type & key,
                           int_type dbindex)
    {
      int socket = get_socket(key);
      send_(socket, makecmd("MOVE") << key << dbindex);
      recv_int_ok_reply_(socket);
    }
    
    void flushdb()
    {
      BOOST_FOREACH(const connection_data & con, connections_)
      {
        send_(con.socket, makecmd("FLUSHDB"));
      }
      
      BOOST_FOREACH(const connection_data & con, connections_)
      {
        recv_ok_reply_(con.socket);
      }
    }
    
    void flushdb(const connection_data & con)
    {
      int socket = con.socket;
      send_(socket, makecmd("FLUSHDB"));
      recv_ok_reply_(socket);
    }
    
    void flushall()
    {
      if( connections_.size() > 1 )
        throw std::runtime_error("feature is not available in cluster mode");
      
      int socket = connections_[0].socket;
      send_(socket, makecmd("FLUSHALL"));
      recv_ok_reply_(socket);
    }
    
    void flushall(const connection_data & con)
    {
      int socket = con.socket;
      send_(socket, makecmd("FLUSHALL"));
      recv_ok_reply_(socket);
    }
    
    int_type sort(const string_type & key,
                  string_vector & out,
                  sort_order order = sort_order_ascending,
                  bool lexicographically = false)
    {
      int socket = get_socket(key);
      makecmd m("SORT");
      m << key << (order == sort_order_ascending ? "ASC" : "DESC");
      if(lexicographically)
        m << "ALPHA";
      
      send_(socket, m);
      return recv_multi_bulk_reply_(socket, out);
    }
    
    int_type sort(const string_type & key,
                  string_vector & out,
                  int_type limit_start,
                  int_type limit_end,
                  sort_order order = sort_order_ascending,
                  bool lexicographically = false)
    {
      makecmd m("SORT");
      m << key
        << "LIMIT"
        << limit_start
        << limit_end
        << (order == sort_order_ascending ? "ASC" : "DESC");
        
      if(lexicographically)
        m << "ALPHA";
      
      send_(m);
      return recv_multi_bulk_reply_(out);
    }
    
    int_type sort(const string_type & key,
                  string_vector & out,
                  const string_type & by_pattern,
                  int_type limit_start,
                  int_type limit_end,
                  const string_vector & get_patterns,
                  sort_order order = sort_order_ascending,
                  bool lexicographically = false)
    {
      int socket = get_socket(key);
      makecmd m("SORT");
      
      m << key
      << "BY"    << by_pattern
      << "LIMIT" << limit_start << limit_end;
      
      string_vector::const_iterator it = get_patterns.begin();
      for ( ; it != get_patterns.end(); ++it)
        m << "GET" << *it;
      
      m << (order == sort_order_ascending ? "ASC" : "DESC")
      << (lexicographically ? "ALPHA" : "");
      
      send_(socket, m);
      return recv_multi_bulk_reply_(socket, out);
    }
    
    void save()
    {
      BOOST_FOREACH(const connection_data & con, connections_)
      {
        send_(con.socket, makecmd("SAVE"));
      }
      
      BOOST_FOREACH(const connection_data & con, connections_)
      {
        recv_ok_reply_(con.socket);
      }
    }
    
    void save(const connection_data & con)
    {
      send_(con.socket, makecmd("SAVE"));
      recv_ok_reply_(con.socket);
    }
    
    void bgsave()
    {
      BOOST_FOREACH(const connection_data & con, connections_)
      {
        send_(con.socket, makecmd("BGSAVE"));
      }
      
      BOOST_FOREACH(const connection_data & con, connections_)
      {
        std::string reply = recv_single_line_reply_(con.socket);
        if(reply != REDIS_STATUS_REPLY_OK && reply != "Background saving started")
          throw protocol_error("Unexpected response on bgsave: '" + reply + "'");
      }
    }
    
    void bgsave(const connection_data & con)
    {
      send_(con.socket, makecmd("BGSAVE"));
      std::string reply = recv_single_line_reply_(con.socket);
      if(reply != REDIS_STATUS_REPLY_OK && reply != "Background saving started")
        throw protocol_error("Unexpected response on bgsave: '" + reply + "'");
    }
    
    time_t lastsave()
    {
      time_t res = 0;
      BOOST_FOREACH(const connection_data & con, connections_)
      {
        int socket = con.socket;
        send_(socket, makecmd("LASTSAVE"));
        time_t cur = recv_int_reply_(socket);
        if(res > 0)
          res = std::min(cur, res);
        else
          res = cur;
      }
      return res;
    }
    
    time_t lastsave(const connection_data & con)
    {
      send_(con.socket, makecmd("LASTSAVE"));
      return recv_int_reply_(con.socket);
    }
    
    void shutdown()
    {
      BOOST_FOREACH(const connection_data & con, connections_)
      {
        int socket = con.socket;
        send_(socket, makecmd("SHUTDOWN"));
        
        // we expected to get a connection_error as redis closes the connection on shutdown command.
        
        try
        {
          recv_ok_reply_(socket);
        }
        catch (connection_error & e)
        {
        }
      }
    }
    
    void shutdown(const connection_data & con)
    {
      send_(con.socket, makecmd("SHUTDOWN"));
      
      // we expected to get a connection_error as redis closes the connection on shutdown command.
      
      try
      {
        recv_ok_reply_(con.socket);
      }
      catch (connection_error & e)
      {
      }
    }
    
    void info(server_info & out)
    {
      if( connections_.size() > 1 )
        throw std::runtime_error("not possible in cluster mode");

      int socket = connections_[0].socket;
      send_(socket, makecmd("INFO"));
      std::string response = recv_bulk_reply_(socket);
      
      if (response.empty())
        throw protocol_error("empty");
      
      string_vector lines;
      split_lines(response, lines);
      if (lines.empty())
        throw protocol_error("empty line for info");
      
      for(string_vector::const_iterator it = lines.begin(); it != lines.end(); ++it)
      {
        const std::string & line = *it;
        string_vector line_parts;
        split(line, ':', line_parts);
        if (line_parts.size() != 2)
          throw protocol_error("unexpected line format for info");
        
        const std::string & key = line_parts[0];
        const std::string & val = line_parts[1];
        
        out.param_map[key] = val;
        
        if (key == "redis_version")
          out.version = val;
        else if (key == "bgsave_in_progress")
          out.bgsave_in_progress = boost::lexical_cast<unsigned long>(val) == 1;
        else if (key == "connected_clients")
          out.connected_clients = boost::lexical_cast<unsigned long>(val);
        else if (key == "connected_slaves")
          out.connected_slaves = boost::lexical_cast<unsigned long>(val);
        else if (key == "used_memory")
          out.used_memory = boost::lexical_cast<unsigned long>(val);
        else if (key == "changes_since_last_save")
          out.changes_since_last_save = boost::lexical_cast<unsigned long>(val);
        else if (key == "last_save_time")
          out.last_save_time = boost::lexical_cast<unsigned long>(val);
        else if (key == "total_connections_received")
          out.total_connections_received = boost::lexical_cast<unsigned long>(val);
        else if (key == "total_commands_processed")
          out.total_commands_processed = boost::lexical_cast<unsigned long>(val);
        else if (key == "uptime_in_seconds")
          out.uptime_in_seconds = boost::lexical_cast<unsigned long>(val);
        else if (key == "uptime_in_days")
          out.uptime_in_days = boost::lexical_cast<unsigned long>(val);
        else if (key == "role")
          out.role = val == "master" ? role_master : role_slave;
        else if (key == "arch_bits")
          out.arch_bits = boost::lexical_cast<unsigned short>(val);
        else if (key == "multiplexing_api")
          out.multiplexing_api = val;
#ifndef NDEBUG // Ignore new/unknown keys in release mode
        else
          std::cerr << "Found unknown info key '" << key << "'" << std::endl;
#endif // NDEBUG
      }
    }

  private:
    base_client(const base_client &);
    base_client & operator=(const base_client &);

    void send_(int socket, const std::string & msg)
    {
#ifndef NDEBUG
      //output_proto_debug(msg, false);
#endif
      
      if (anetWrite(socket, const_cast<char *>(msg.data()), msg.size()) == -1)
        throw connection_error(strerror(errno));
    }
    
    std::string recv_single_line_reply_(int socket)
    {
      std::string line = read_line(socket);
      
      #ifndef NDEBUG
      //output_proto_debug(line);
      #endif
      
      if (line.empty())
        throw protocol_error("empty single line reply");
      
      if (line.find(REDIS_PREFIX_STATUS_REPLY_ERROR) == 0)
      {
        std::string error_msg = line.substr( strlen(REDIS_PREFIX_STATUS_REPLY_ERROR) );
        if (error_msg.empty())
          error_msg = "unknown error";
        throw protocol_error(error_msg);
      }
      
      if (line[0] != REDIS_PREFIX_STATUS_REPLY_VALUE)
        throw protocol_error("unexpected prefix for status reply");
      
      return line.substr(1);
    }
    
    void recv_ok_reply_(int socket)
    {
      if (recv_single_line_reply_(socket) != REDIS_STATUS_REPLY_OK)
        throw protocol_error("expected OK response");
    }
    
    int_type recv_bulk_reply_(int socket, char prefix)
    {
      std::string line = read_line(socket);
      
#ifndef NDEBUG
      //output_proto_debug(line);
#endif
      
      if (line[0] != prefix)
      {
#ifndef NDEBUG
        std::cerr << "unexpected prefix for bulk reply (expected '" << prefix << "' but got '" << line[0] << "')" << std::endl;
#endif // NDEBUG
        throw protocol_error("unexpected prefix for bulk reply");
      }
      
      return boost::lexical_cast<int_type>(line.substr(1));
    }
    
    std::string recv_bulk_reply_(int socket)
    {
      int_type length = recv_bulk_reply_(socket, REDIS_PREFIX_SINGLE_BULK_REPLY );
      
      if (length == -1)
        return missing_value();
      
      int_type real_length = length + 2;    // CRLF
      
      std::string data = read_n(socket, real_length);
      
#ifndef NDEBUG
      //output_proto_debug(data.substr(0, data.length()-2));
#endif
      
      if (data.empty())
        throw protocol_error("invalid bulk reply data; empty");
      
      if (data.length() != static_cast<std::string::size_type>(real_length))
        throw protocol_error("invalid bulk reply data; data of unexpected length");
      
      data.erase(data.size() - 2);
      
      return data;
    }
    
    int_type recv_multi_bulk_reply_(int socket, string_vector & out)
    {
      int_type length = recv_bulk_reply_(socket, REDIS_PREFIX_MULTI_BULK_REPLY);
      
      if (length == -1)
        throw key_error("no such key");

      out.reserve( out.size()+length );
      
      for (int_type i = 0; i < length; ++i)
        out.push_back(recv_bulk_reply_(socket));
      
      return length;
    }
    
    int_type recv_multi_bulk_reply_(int socket, string_set & out)
    {
      int_type length = recv_bulk_reply_(socket, REDIS_PREFIX_MULTI_BULK_REPLY);
      
      if (length == -1)
        throw key_error("no such key");
      
      for (int_type i = 0; i < length; ++i)
        out.insert(recv_bulk_reply_(socket));
      
      return length;
    }
    
    int_type recv_int_reply_(int socket)
    {
      std::string line = read_line(socket);
      
#ifndef NDEBUG
      //output_proto_debug(line);
#endif
      
      if (line.empty())
        throw protocol_error("invalid integer reply; empty");
      
      if (line[0] != REDIS_PREFIX_INT_REPLY)
        throw protocol_error("unexpected prefix for integer reply");
      
      return boost::lexical_cast<int_type>(line.substr(1));
    }
    
    void recv_int_ok_reply_(int socket)
    {
      if (recv_int_reply_(socket) != 1)
        throw protocol_error("expecting int reply of 1");
    }
    
    inline int get_socket(const string_type & key)
    {
      size_t con_count = connections_.size();
      if(con_count == 1)
        return connections_[0].socket;
      size_t idx = hasher_( key, static_cast<const std::vector<connection_data> &>(connections_) );
      return connections_[idx].socket;
    }

    int get_socket(const string_vector & keys)
    {
      assert( !keys.empty() );
      
      int socket = -1;
      for(size_t i=0; i < keys.size(); i++)
      {
        int cur_socket = get_socket(keys[i]);
        if(i > 0 && socket != cur_socket)
          throw std::runtime_error("not possible in cluster mode");
        
        socket = cur_socket;
      }

      return socket;
    }

  private:
    std::vector<connection_data> connections_;
    //int socket_;
    CONSISTENT_HASHER hasher_;
  };
  
  struct default_hasher
  {
    inline size_t operator()(const std::string & key, const std::vector<connection_data> & connections)
    {
      return boost::hash<std::string>()(key) % connections.size();
    }
  };
  
  typedef base_client<default_hasher> client;

  class shared_value
  {
  public:
    shared_value(client & client_conn, const client::string_type & key)
     : client_conn_(&client_conn),
       key_(key)
    {
    }

    virtual ~shared_value()
    {
    }

    inline const client::string_type & key() const
    {
      return key_;
    }

    bool exists() const
    {
      return client_conn_->exists(key_);
    }

    void del()
    {
      return client_conn_->del(key_);
    }

    void rename(const client::string_type & new_name)
    {
      client_conn_->rename(key_, new_name);
      key_ = new_name;
    }
    
    bool renamenx(const client::string_type & new_name)
    {
      if( client_conn_->renamenx(key_, new_name) )
      {
        key_ = new_name;
        return true;
      }

      return false;
    }

    void expire(unsigned int secs)
    {
      client_conn_->expire(key_, secs);
    }

    int ttl() const
    {
      return client_conn_->ttl(key_);
    }

    void move(client::int_type dbindex)
    {
      client_conn_->move(key_, dbindex);
    }

    client::datatype type() const
    {
      return client_conn_->type(key_);
    }

  protected:
    redis::client* client_conn_;
    
  private:
    shared_value & operator=(const shared_value &)
    {
      return *this;
    }
    
    client::string_type key_;
  };

  class shared_string : public shared_value
  {
  public:
    shared_string(client & client_conn, const client::string_type & key)
     : shared_value(client_conn, key)
    {
    }

    shared_string(redis::client & client_conn, const client::string_type & key, const client::string_type & default_value)
     : shared_value(client_conn, key)
    {
      setnx(default_value);
    }

    operator client::string_type() const
    {
      return client_conn_->get(key());
    }

    inline client::string_type str() const
    {
      return *this;
    }

    shared_string & operator=(const client::string_type & value)
    {
      client_conn_->set(key(), value);
      return *this;
    }

    shared_string & operator=(const shared_string & other_str)
    {
      if( key() != other_str.key() )
        *this = other_str.str();
      
      return *this;
    }

    client::string_type getset(const client::string_type & new_value)
    {
      return client_conn_->getset(key(), new_value);
    }

    bool setnx(const client::string_type & value)
    {
      return client_conn_->setnx(key(), value);
    }

    void setex(const client::string_type & value, unsigned int secs)
    {
      client_conn_->setex(key(), value, secs);
    }

    size_t append(const client::string_type & value)
    {
      return client_conn_->append(key(), value);
    }

    shared_string & operator+=(const client::string_type & value)
    {
      append(value);
      return *this;
    }

    client::string_type substr(int start, int end) const
    {
      return client_conn_->substr(key(), start, end);
    }
  };

  class shared_int : public shared_value
  {
  public:
    shared_int(client & client_conn, const client::string_type & key)
     : shared_value(client_conn, key)
    {
    }
    
    shared_int(redis::client & client_conn, const client::string_type & key, const client::int_type & default_value)
     : shared_value(client_conn, key)
    {
      setnx(default_value);
    }

    shared_int & operator=(client::int_type val)
    {
      client_conn_->set(key(), boost::lexical_cast<client::string_type>(val));
      return *this;
    }
    
    shared_int & operator=(const shared_int & other)
    {
      if(key() != other.key())
        client_conn_->set(key(), boost::lexical_cast<client::string_type>(other.to_int()));
      return *this;
    }
    
    operator client::int_type() const
    {
      return to_int_type( client_conn_->get(key()) );
    }

    client::int_type to_int() const
    {
      return *this;
    }
    
    bool setnx(const client::int_type & value)
    {
      return client_conn_->setnx(key(), boost::lexical_cast<client::string_type>(value) );
    }
    
    void setex(const client::int_type & value, unsigned int secs)
    {
      client_conn_->setex(key(), boost::lexical_cast<client::string_type>(value), secs);
    }
    
    client::int_type operator++()
    {
      return client_conn_->incr(key());
    }
    
    client::int_type operator++(int)
    {
      return client_conn_->incr(key()) - 1;
    }
    
    client::int_type operator--()
    {
      return client_conn_->decr(key());
    }
    
    client::int_type operator--(int)
    {
      return client_conn_->decr(key()) + 1;
    }

    client::int_type operator+=(client::int_type val)
    {
      return client_conn_->incrby(key(), val);
    }
    
    client::int_type operator-=(client::int_type val)
    {
      return client_conn_->decrby(key(), val);
    }

  private:
    static client::int_type to_int_type(const client::string_type & val)
    {
      try
      {
        return boost::lexical_cast<client::int_type>( val );
      }
      catch(boost::bad_lexical_cast & e)
      {
        throw value_error("value is not of integer type");
      }
    }
  };

  class shared_list : public shared_value
  {
  public:
    shared_list(client & client_conn, const client::string_type & key)
     : shared_value(client_conn, key)
    {
    }

    void push_back(const client::string_type & value)
    {
      client_conn_->rpush(key(), value);
    }
    
    void push_front(const client::string_type & value)
    {
      client_conn_->lpush(key(), value);
    }

    client::string_type pop_back()
    {
      return client_conn_->rpop(key());
    }

    client::string_type blocking_pop_back(client::int_type timeout = 0)
    {
      return client_conn_->brpop(key(), timeout);
    }

    client::string_type pop_front()
    {
      return client_conn_->lpop(key());
    }
    
    client::string_type blocking_pop_front(client::int_type timeout = 0)
    {
      return client_conn_->blpop(key(), timeout);
    }
    
    size_t size() const
    {
      return client_conn_->llen(key());
    }

    client::string_vector range(client::int_type begin = 0, client::int_type end = -1) const
    {
      client::string_vector res;
      client_conn_->lrange(key(), begin, end, res);
      return res;
    }

    inline client::string_vector to_vector() const
    {
      return range();
    }

    void trim(client::int_type begin, client::int_type end = -1)
    {
      client_conn_->ltrim(key(), begin, end);
    }
    
    client::string_type operator[](client::int_type index)
    {
      return client_conn_->lindex(key(), index);
    }

    void set(client::int_type index, const client::string_type & value)
    {
      client_conn_->lset(key(), index, value);
    }
  };

  /**
   * This class works on 'redis sets'. For best matching the stl/boost naming conventions it is called
   * shared_unordered_set and not shared_set. 
   */
  class shared_unordered_set : public shared_value
  {
  public:
    shared_unordered_set(client & client_conn, const client::string_type & key)
     : shared_value(client_conn, key)
    {
    }
    
    void insert(const client::string_type & value)
    {
      client_conn_->sadd(key(), value);
    }
    
    void erase(const client::string_type & value)
    {
      client_conn_->srem(key(), value);
    }
    
    void clear()
    {
      del();
    }
    
    client::int_type count() const
    {
      return client_conn_->scard(key());
    }
    
    client::string_type pop_random()
    {
      return client_conn_->spop(key());
    }

    client::string_type get_random() const
    {
      return client_conn_->srandmember(key());
    }

    bool contains(const client::string_type & value) const
    {
      return client_conn_->sismember(key(), value);
    }
  };
  
  /**
   * This class works on 'redis sorted sets'. For best matching the stl/boost naming conventions it is called
   * shared_set.
   */
  class shared_set : public shared_value
  {
  public:
    shared_set(client & client_conn, const client::string_type & key)
     : shared_value(client_conn, key)
    {
    }
  };
}

inline bool operator==(const redis::shared_string & sh_str, const redis::client::string_type & str)
{
  return sh_str.str() == str;
}

inline bool operator!=(const redis::shared_string & sh_str, const redis::client::string_type & str)
{
  return sh_str.str() != str;
}

template <typename ch, typename char_traits>
std::basic_ostream<ch, char_traits>& operator<<(std::basic_ostream<ch, char_traits> & os, const redis::shared_string & sh_str)
{
  return os << sh_str.str();
}

template <typename ch, typename char_traits>
std::basic_istream<ch, char_traits>& operator>>(std::basic_istream<ch, char_traits> & is, redis::shared_string & sh_str)
{
  redis::client::string_type s_val;
  is >> s_val;
  sh_str = s_val;
  return is;
}

#endif // REDISCLIENT_H
