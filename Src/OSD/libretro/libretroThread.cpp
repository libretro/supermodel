#include <OSD/Thread.h>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <atomic>
#include "Supermodel.h"

// --- Helper Structure for Semaphore (since C++17 lacks std::counting_semaphore) ---
struct SemaphoreImpl {
    std::mutex mtx;
    std::condition_variable cv;
    int count;

    SemaphoreImpl(int initial) : count(initial) {}
};

// --- CThread Static Methods ---

void CThread::Sleep(UINT32 ms)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

UINT32 CThread::GetTicks()
{
    auto now = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch());
    return static_cast<UINT32>(ms.count());
}

CThread* CThread::CreateThread(const std::string &name, ThreadStart start, void* startParam)
{
    try {
        // Construct a new std::thread and store it in m_impl
        std::thread* t = new std::thread(start, startParam);
        return new CThread(name, t);
    } catch (...) {
        return nullptr;
    }
}

CSemaphore *CThread::CreateSemaphore(UINT32 initVal)
{
    return new CSemaphore(new SemaphoreImpl(static_cast<int>(initVal)));
}

CCondVar *CThread::CreateCondVar()
{
    // std::condition_variable_any is used to match the flexibility of SDL_cond
    return new CCondVar(new std::condition_variable_any());
}

CMutex *CThread::CreateMutex()
{
    // SDL_mutex is recursive by default; Supermodel relies on this behavior
    return new CMutex(new std::recursive_mutex());
}

const char *CThread::GetLastError()
{
    return "C++ Standard Library Thread Error";
}

// --- CThread Member Methods ---

CThread::CThread(const std::string &name, void *impl)
  : m_name(name),
    m_impl(impl)
{
}

CThread::~CThread()
{
  if (nullptr != m_impl)
  {
    std::thread* t = static_cast<std::thread*>(m_impl);
    if (t->joinable()) {
        ErrorLog("Runaway thread error. A thread was not properly halted: %s. Detaching...\n", GetName().c_str());
        t->detach();
    }
    delete t;
    m_impl = nullptr;
  }
}

const std::string &CThread::GetName() const
{
  return m_name;
}

UINT32 CThread::GetId()
{
    std::thread* t = static_cast<std::thread*>(m_impl);
    // Hash the thread ID to return a UINT32 representation
    return static_cast<UINT32>(std::hash<std::thread::id>{}(t->get_id()));
}

int CThread::Wait()
{
    if (m_impl == nullptr)
        return -1;

    std::thread* t = static_cast<std::thread*>(m_impl);
    if (t->joinable()) {
        t->join();
    }
    
    delete t;
    m_impl = nullptr;
    return 0; // Standard C++ doesn't return function exit code via join()
}

// --- CSemaphore Member Methods ---

CSemaphore::CSemaphore(void *impl) : m_impl(impl) {}

CSemaphore::~CSemaphore()
{
    delete static_cast<SemaphoreImpl*>(m_impl);
}

UINT32 CSemaphore::GetValue()
{
    SemaphoreImpl* sem = static_cast<SemaphoreImpl*>(m_impl);
    std::lock_guard<std::mutex> lock(sem->mtx);
    return static_cast<UINT32>(sem->count);
}

bool CSemaphore::Wait()
{
    SemaphoreImpl* sem = static_cast<SemaphoreImpl*>(m_impl);
    std::unique_lock<std::mutex> lock(sem->mtx);
    sem->cv.wait(lock, [sem]() { return sem->count > 0; });
    sem->count--;
    return true;
}

bool CSemaphore::Post()
{
    SemaphoreImpl* sem = static_cast<SemaphoreImpl*>(m_impl);
    {
        std::lock_guard<std::mutex> lock(sem->mtx);
        sem->count++;
    }
    sem->cv.notify_one();
    return true;
}

// --- CCondVar Member Methods ---

CCondVar::CCondVar(void *impl) : m_impl(impl) {}

CCondVar::~CCondVar()
{
    delete static_cast<std::condition_variable_any*>(m_impl);
}

bool CCondVar::Wait(CMutex *mutex)
{
    std::condition_variable_any* cv = static_cast<std::condition_variable_any*>(m_impl);
    std::recursive_mutex* mtx = static_cast<std::recursive_mutex*>(mutex->m_impl);
    
    // std::condition_variable_any works directly with std::recursive_mutex
    cv->wait(*mtx);
    return true;
}

bool CCondVar::Signal()
{
    static_cast<std::condition_variable_any*>(m_impl)->notify_one();
    return true;
}

bool CCondVar::SignalAll()
{
    static_cast<std::condition_variable_any*>(m_impl)->notify_all();
    return true;
}

// --- CMutex Member Methods ---

CMutex::CMutex(void *impl) : m_impl(impl) {}

CMutex::~CMutex()
{
    delete static_cast<std::recursive_mutex*>(m_impl);
}

bool CMutex::Lock()
{
    static_cast<std::recursive_mutex*>(m_impl)->lock();
    return true;
}

bool CMutex::Unlock()
{
    static_cast<std::recursive_mutex*>(m_impl)->unlock();
    return true;
}