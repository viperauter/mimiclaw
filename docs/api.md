====================================================================
Cross-Thread Event Framework – Message Flow & Thread Architecture
====================================================================

Threads
-------

1. Reactor Thread (Event Loop)
    - Only calls poll (mg_mgr_poll / uv_run / event_base_loop)
    - Processes send queue from workers
    - Executes timers
    - Handles IO events

2. Worker Threads (Dispatcher Pool)
    - Consume recv events from reactor
    - Parse protocol, run business logic
    - Generate send events for reactor

3. Optional Main Thread
    - Initialize runtime / reactor
    - Can post tasks to workers via event_bus

====================================================================
Message Flow
====================================================================

        +---------------------------+
        | Worker Thread 0           |
        |---------------------------|
        | Business Logic            |
        | Generate EVENT_SEND        |
        +-------------+-------------+
                      |
                      v (event_bus_post_send)
        +---------------------------+
        | Event Bus Send Queue      |
        | Thread-Safe Queue         |
        +-------------+-------------+
                      |
                      v (wakeup)
        +---------------------------+
        | Reactor Thread            |
        | mg_mgr_poll / libuv loop  |
        | drain send queue          |
        | call mg_send()            |
        +-------------+-------------+
                      |
         +------------+------------+
         | IO Event / Data Arrival |
         +------------+------------+
                      |
                      v
        +---------------------------+
        | Event Bus Recv Queue      |
        | Thread-Safe Queue         |
        +-------------+-------------+
                      |
                      v
        +---------------------------+
        | Dispatcher / Worker Pool  |
        | Consume recv events       |
        | Call registered handlers  |
        +---------------------------+

Notes:
- Worker threads never directly touch sockets.
- Reactor never blocks on business logic.
- Event bus handles wakeup, zero-copy buffer transfer, and thread safety.

====================================================================
Event Bus API Skeleton
====================================================================

typedef struct event_bus event_bus_t;

/* Create/destroy bus */
event_bus_t *event_bus_create(size_t queue_capacity);
void event_bus_destroy(event_bus_t *bus);

/* Post events */
int event_bus_post_recv(event_bus_t *bus, event_type_t type,
                        uint64_t conn_id, conn_type_t conn_type,
                        io_buf_t *buf, uint64_t user_data,
                        uint32_t flags);
int event_bus_post_send(event_bus_t *bus, uint64_t conn_id,
                        conn_type_t conn_type, io_buf_t *buf,
                        uint32_t flags);
int event_bus_post_close(event_bus_t *bus, uint64_t conn_id,
                         conn_type_t conn_type, uint32_t flags);
int event_bus_post_error(event_bus_t *bus, uint64_t conn_id,
                         conn_type_t conn_type, int32_t error_code,
                         uint64_t user_data, uint32_t flags);

/* Drain queues (called from reactor) */
void event_bus_drain_recv(event_bus_t *bus);
void event_bus_drain_send(event_bus_t *bus);

/* Set wakeup callback */
typedef void (*event_bus_wakeup_fn)(void *arg);
void event_bus_set_wakeup(event_bus_t *bus, event_bus_wakeup_fn fn, void *arg);

====================================================================
Dispatcher API Skeleton
====================================================================

typedef void (*dispatcher_handler_t)(event_msg_t *msg, void *user_data);

typedef struct dispatcher dispatcher_t;

/* Create/destroy dispatcher */
dispatcher_t *dispatcher_create(size_t worker_count);
void dispatcher_destroy(dispatcher_t *disp);

/* Register handler per connection type */
int dispatcher_register_handler(dispatcher_t *disp,
                                conn_type_t conn_type,
                                dispatcher_handler_t handler,
                                void *user_data);

/* Start/stop worker threads */
int dispatcher_start(dispatcher_t *disp);
void dispatcher_stop(dispatcher_t *disp);

/* Drain send queue (reactor calls) */
void dispatcher_drain_send(dispatcher_t *disp);

====================================================================
Runtime / Reactor Skeleton
====================================================================

typedef struct runtime runtime_t;

int runtime_init(runtime_t *rt);
int runtime_start(runtime_t *rt);
void runtime_stop(runtime_t *rt);
void runtime_deinit(runtime_t *rt);

/* Accessors */
runtime_t *runtime_get_global(void);
event_bus_t *runtime_get_event_bus(runtime_t *rt);
dispatcher_t *runtime_get_dispatcher(runtime_t *rt);
void *runtime_get_reactor(runtime_t *rt);

====================================================================
Zero-Copy Buffer Handling
====================================================================

typedef struct io_buf io_buf_t;

io_buf_t *io_buf_create(size_t size);
void io_buf_ref(io_buf_t *buf);
void io_buf_unref(io_buf_t *buf);
uint8_t *io_buf_data(io_buf_t *buf);
size_t io_buf_len(io_buf_t *buf);

Rules:
- Worker thread posts buffer -> refcount +1
- Reactor drains buffer -> sends data -> unref
- Receive buffer -> dispatcher -> worker -> unref after processing

====================================================================
Key Design Principles
====================================================================

1. Reactor thread: poll-only, no blocking operations
2. Worker threads: business logic only, never touch sockets
3. Event bus: cross-thread safe, wakeup support, zero-copy
4. Dispatcher: routes messages by connection type
5. Connection state: check before sending
6. Timer events integrated into reactor
7. Naming: module_object_action (event_bus_post_send, dispatcher_register_handler)
8. Scalability: easy to add more workers or multiple reactors