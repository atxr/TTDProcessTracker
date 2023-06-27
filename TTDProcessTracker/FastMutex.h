#pragma once
#include <ntddk.h>

template<typename TLock>
class AutoLock {
public:
	AutoLock(TLock& lock) : m_lock(lock) {
		m_lock.Lock();
	}

	~AutoLock() {
		m_lock.Unlock();
	}

private:
	TLock& m_lock;
};;

class FastMutex {
public:
	void Init();
	void Lock();
	void Unlock();

private:
	FAST_MUTEX m_mutex;
};
