#ifndef IDGEN_HPP
#define IDGEN_HPP

#include <string>
#include <random>

class IDGen
{
public:
    IDGen() = delete;

    static std::string GenerateID(int len)
    {
        // thread_local engine avoids both seeding-once-globally and data races.
        // NOTE: must be a SMALL engine. std::mt19937 carries ~2.5KB of state, and
        // thread_local state lives in the per-task TLS block that is copied onto every
        // task's stack. On ESP32 the IPC tasks have ~1.3KB stacks, so an mt19937 TLS
        // block overran them and corrupted an adjacent TCB -> FreeRTOS scheduler crash
        // before app_main (bootloop on all transports). minstd_rand is ~8 bytes. (#74)
        thread_local std::minstd_rand rng{std::random_device{}()};
        thread_local std::uniform_int_distribution<int> dist{
            0, static_cast<int>(sizeof(alphanum) - 2)};

        std::string id;
        id.reserve(len);
        for (int i = 0; i < len; ++i)
        {
            id += alphanum[dist(rng)];
        }
        return id;
    }

private:
    static constexpr char alphanum[] =
        "0123456789"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz";
};

#endif
