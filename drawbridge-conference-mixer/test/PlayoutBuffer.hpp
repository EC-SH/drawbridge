// Minimal stub matching drawbridge's PlayoutBuffer interface, for standalone
// compile-verification of MixBus. The real one lives in src/SIP/PlayoutBuffer.hpp.
#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>
#include <mutex>
class PlayoutBuffer {
public:
    size_t write(const int16_t* s, size_t n) {
        std::lock_guard<std::mutex> lk(_m);
        for (size_t i=0;i<n;++i) _q.push_back(s[i]);
        return n;
    }
    bool read(int16_t* out, size_t n) {
        std::lock_guard<std::mutex> lk(_m);
        if (_q.size() < n) { for (size_t i=0;i<n;++i) out[i]=0; return false; }
        for (size_t i=0;i<n;++i){ out[i]=_q[i]; }
        _q.erase(_q.begin(), _q.begin()+n);
        return true;
    }
    void clear(){ std::lock_guard<std::mutex> lk(_m); _q.clear(); }
private:
    std::vector<int16_t> _q;
    std::mutex _m;
};
