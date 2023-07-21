class Communicator
{
public:
	int init(size_t poller_threads, size_t handler_threads);
	void deinit();

	int request(CommSession *session, CommTarget *target);
	int reply(CommSession *session);

	int push(const void *buf, size_t size, CommSession *session);

	int bind(CommService *service);
	void unbind(CommService *service);

	int sleep(SleepSession *session);

	int io_bind(IOService *service);
	void io_unbind(IOService *service);

public:
	int is_handler_thread() const;
	int increase_handler_thread();

private:
	struct __mpoller *mpoller;
	struct __msgqueue *msgqueue;
	struct __thrdpool *thrdpool;
	int stop_flag;

public:
	virtual ~Communicator() { }
};