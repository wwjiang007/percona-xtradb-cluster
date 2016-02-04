/*
   Copyright (c) 2013, 2015, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA
*/

#ifndef CONNECTION_HANDLER_MANAGER_INCLUDED
#define CONNECTION_HANDLER_MANAGER_INCLUDED

#include "my_global.h"
#include "mysql/psi/mysql_thread.h"  // mysql_mutex_t
#include "connection_handler.h"      // Connection_handler

class Channel_info;
class THD;


/**
  Functions to notify interested connection handlers
  of events like begining of wait and end of wait and post-kill
  notification events.
*/
struct THD_event_functions
{
  void (*thd_wait_begin)(THD* thd, int wait_type);
  void (*thd_wait_end)(THD* thd);
  void (*post_kill_notification)(THD* thd);
};


/**
  This is a singleton class that provides various connection management
  related functionalities, most importantly dispatching new connections
  to the currently active Connection_handler.
*/
class Connection_handler_manager
{
  // Singleton instance to Connection_handler_manager
  static Connection_handler_manager* m_instance;

  static mysql_mutex_t LOCK_connection_count;

  // Pointer to current connection handler in use
  Connection_handler* m_connection_handler;
  // Pointer to extra connection handler
  Connection_handler* m_extra_connection_handler;
  // Pointer to saved connection handler
  Connection_handler* m_saved_connection_handler;
  // Saved scheduler_type
  ulong m_saved_thread_handling;

  // Status variables
  ulong m_aborted_connects;
  ulong m_connection_errors_max_connection; // Protected by LOCK_connection_count


  /**
    Constructor to instantiate an instance of this class.
  */
  Connection_handler_manager(Connection_handler *connection_handler,
                             Connection_handler *extra_connection_handler)
  : m_connection_handler(connection_handler),
    m_extra_connection_handler(extra_connection_handler),
    m_saved_connection_handler(NULL),
    m_saved_thread_handling(0),
    m_aborted_connects(0),
    m_connection_errors_max_connection(0)
  { }

  ~Connection_handler_manager()
  {
    delete m_connection_handler;
    delete m_extra_connection_handler;
    if (m_saved_connection_handler)
      delete m_saved_connection_handler;
  }

  /* Make this class non-copyable */
  Connection_handler_manager(const Connection_handler_manager&);
  Connection_handler_manager& operator=(const Connection_handler_manager&);

public:
  /**
    thread_handling enumeration.

    The default of --thread-handling is the first one in the
    thread_handling_names array, this array has to be consistent with
    the order in this array, so to change default one has to change the
    first entry in this enum and the first entry in the
    thread_handling_names array.

    @note The last entry of the enumeration is also used to mark the
    thread handling as dynamic. In this case the name of the thread
    handling is fetched from the name of the plugin that implements it.
  */
  enum scheduler_types
  {
    SCHEDULER_ONE_THREAD_PER_CONNECTION=0,
    SCHEDULER_NO_THREADS,
    SCHEDULER_THREAD_POOL,
    SCHEDULER_TYPES_COUNT
  };

  // Status variables. Must be static as they are used by the signal handler.
  static uint connection_count;          // Protected by LOCK_connection_count
  static uint extra_connection_count;    // Protected by LOCK_connection_count
  static ulong max_used_connections;     // Protected by LOCK_connection_count
  static ulong max_used_connections_time;// Protected by LOCK_connection_count

  // System variable
  static ulong thread_handling;

  // Functions for lock wait and post-kill notification events
  static THD_event_functions *event_functions;
  // Saved event functions
  static THD_event_functions *saved_event_functions;

  /**
     Maximum number of threads that can be created by the current
     connection handler. Must be static as it is used by the signal handler.
  */
  static uint max_threads;

  /**
    Singleton method to return an instance of this class.
  */
  static Connection_handler_manager* get_instance()
  {
    DBUG_ASSERT(m_instance != NULL);
    return m_instance;
  }

  /**
    Initialize the connection handler manager.
    Must be called before get_instance() can be used.

    @return true if initialization failed, false otherwise.
  */
  static bool init();

  /**
    Destroy the singleton instance.
  */
  static void destroy_instance();

  /**
    Check if the current number of connections are below or equal
    the value given by the max_connections server system variable.

    @param extra_port_connection true if it is the extra port connections which
    need to be validated

    @return true if a new connection can be accepted, false otherwise.
  */
  bool valid_connection_count(bool extra_port_connection);

  /**
    Increment connection count if max_connections is not exceeded.

    @param    extra_port_connection true if it is the extra connection count
    which needs to be checked and bumped

    @retval
      true   max_connections NOT exceeded
      false  max_connections reached
  */
  bool check_and_incr_conn_count(bool extra_port_connection);

  /**
    Reset the max_used_connections counter to the number of current
    connections.
  */
  static void reset_max_used_connections();

  /**
    Decrease the number of current connections.

    @param extra_port_connection true if it is the extra connection count which
    needs to be decreased
  */
  static void dec_connection_count(bool extra_port_connection)
  {
#ifdef WITH_WSREP
    /*
      Do not decrement when its wsrep system thread. wsrep_applier is set for
      applier as well as rollbacker threads.
    */
    if (!thd->wsrep_applier)
    {
#endif /* WITH_WSREP */

    mysql_mutex_lock(&LOCK_connection_count);
    if (extra_port_connection)
      extra_connection_count--;
    else
      connection_count--;
    mysql_mutex_unlock(&LOCK_connection_count);

#ifdef WITH_WSREP
    }
#endif
  }

  void inc_aborted_connects()
  {
    m_aborted_connects++;
  }

  ulong aborted_connects() const
  {
    return m_aborted_connects;
  }

  /**
    @note This is a dirty read.
  */
  ulong connection_errors_max_connection() const
  {
    return m_connection_errors_max_connection;
  }

  /**
    Dynamically load a connection handler implemented as a plugin.
    The current connection handler will be saved so that it can
    later be restored by unload_connection_handler().
  */
  void load_connection_handler(Connection_handler* conn_handler);

  /**
    Unload the connection handler previously loaded by
    load_connection_handler(). The previous connection handler will
    be restored.

    @return true if unload failed (no previous connection handler was found).
  */
  bool unload_connection_handler();

  /**
    Process a new incoming connection.

    @param channel_info    Pointer to Channel_info object containing
                           connection channel information.
  */
  void process_new_connection(Channel_info* channel_info);
};
#endif // CONNECTION_HANDLER_MANAGER_INCLUDED.
