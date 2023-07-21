class CommMessageIn 
{
private:

    virtual int append(const void *buf, size_t *size) = 0; 

private:

    struct CommConnEntry *entry;

public:

    virtual ~CommMessageIn() { }

    friend class Communicator;
};

class ProtocolMessage :public CommMessageIn
{
private:

	virtual int append(const void *buf, size_t *size)
	{
		return 0;
	}

};