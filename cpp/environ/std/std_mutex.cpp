#include "PlatformMutex.h"

#include <mutex>
#include <thread>

struct tTJSCriticalSectionImpl
{
    std::mutex _mutex;
    std::thread::id _tid;
    bool lock();
    void unlock();
};

bool tTJSCriticalSectionImpl::lock()
{
    std::thread::id id = std::this_thread::get_id();
    if (_tid == id)
        return false;
    _mutex.lock();
    _tid = id;
    return true;
}

void tTJSCriticalSectionImpl::unlock()
{
    _tid = std::thread::id();
    _mutex.unlock();
}

bool tTJSCriticalSection::lock()
{
    return _impl->lock();
}

void tTJSCriticalSection::unlock()
{
    _impl->unlock();
}

tTJSCriticalSection::tTJSCriticalSection()
{
    _impl = new tTJSCriticalSectionImpl;
}

tTJSCriticalSection::~tTJSCriticalSection()
{
    delete _impl;
}