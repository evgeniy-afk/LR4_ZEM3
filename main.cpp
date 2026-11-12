#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <shared_mutex>
#include <random>
#include <chrono>
#include <iomanip>
#include <filesystem>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif


class Data {
public:
    static constexpr int M = 2;

    int get(int i) const {
        std::shared_lock lk(mtx(i));          
        return f[i];
    }

    void set(int i, int v) {
        std::unique_lock lk(mtx(i));         
        f[i] = v;
    }

    operator std::string() const {
        std::shared_lock l0(m0, std::defer_lock);
        std::shared_lock l1(m1, std::defer_lock);
        std::lock(l0, l1);                    
        return "[" + std::to_string(f[0]) + ", " + std::to_string(f[1]) + "]";
    }

private:
    std::shared_mutex& mtx(int i) const { return i == 0 ? m0 : m1; }

    int f[M]{};                               
    alignas(64) mutable std::shared_mutex m0; 
    alignas(64) mutable std::shared_mutex m1; 
};


enum class OpType : unsigned char { Read, Write, Str };
struct Op { OpType type; int field; int value; };

struct Freq { double r0, w0, r1, w1, s; };

constexpr int N = 2'400'000;   
constexpr int CHUNKS = 6;           
constexpr int REPEATS = 5;           


void generate(const std::string& path, const Freq& fr, int count, unsigned seed) {
    std::mt19937 gen(seed);
    std::discrete_distribution<int> dist({ fr.r0, fr.w0, fr.r1, fr.w1, fr.s });
    std::uniform_int_distribution<int> val(1, 100);
    std::ofstream out(path);
    for (int k = 0; k < count; ++k) {
        switch (dist(gen)) {
        case 0: out << "read 0\n"; break;
        case 1: out << "write 0 " << val(gen) << '\n'; break;
        case 2: out << "read 1\n"; break;
        case 3: out << "write 1 " << val(gen) << '\n'; break;
        default: out << "string\n"; break;
        }
    }
}

std::vector<Op> load(const std::string& path) {
    std::ifstream in(path);
    std::vector<Op> ops;
    ops.reserve(N / CHUNKS);
    std::string cmd;
    while (in >> cmd) {
        if (cmd == "read") { int i; in >> i;           ops.push_back({ OpType::Read,  i, 0 }); }
        else if (cmd == "write") { int i, v; in >> i >> v;   ops.push_back({ OpType::Write, i, v }); }
        else if (cmd == "string")                          ops.push_back({ OpType::Str,   0, 0 });
    }
    return ops;
}


void worker(Data& d, const std::vector<const std::vector<Op>*>& myChunks, std::size_t& sink) {
    std::size_t local = 0;                    
    for (const auto* chunk : myChunks)
        for (const Op& op : *chunk) {
            switch (op.type) {
            case OpType::Read:  local += d.get(op.field); break;
            case OpType::Write: d.set(op.field, op.value); break;
            case OpType::Str:   local += static_cast<std::string>(d).size(); break;
            }
        }
    sink = local;
}

std::size_t g_sink = 0;

double measure(const std::vector<std::vector<Op>>& chunks, int nthreads) {
    Data d;
    const int per = CHUNKS / nthreads;
    std::vector<std::vector<const std::vector<Op>*>> parts(nthreads);
    for (int t = 0; t < nthreads; ++t)
        for (int c = t * per; c < (t + 1) * per; ++c)
            parts[t].push_back(&chunks[c]);

    std::vector<std::size_t> sinks(nthreads);
    auto start = std::chrono::steady_clock::now();
    {
        std::vector<std::jthread> threads;
        for (int t = 0; t < nthreads; ++t)
            threads.emplace_back(worker, std::ref(d), std::cref(parts[t]), std::ref(sinks[t]));
    }
    auto end = std::chrono::steady_clock::now();

    for (auto s : sinks) g_sink += s;
    return std::chrono::duration<double, std::milli>(end - start).count();
}

int main() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    const char  names[3] = { 'a', 'b', 'c' };
    const char* labels[3] = { "а) варіант  ", "б) рівні    ", "в) невідп.  " };
    const Freq  freqs[3] = {
        { 25,  5,  5, 25, 40 },   
        { 20, 20, 20, 20, 20 },   
        {  5, 45, 40,  5,  5 }    
    };

    std::filesystem::create_directories("data");
    std::cout << "Генерація файлів...\n";
    for (int c = 0; c < 3; ++c)
        for (int k = 0; k < CHUNKS; ++k)
            generate("data/case_" + std::string(1, names[c]) + "_" + std::to_string(k) + ".txt",
                freqs[c], N / CHUNKS, 1000u * c + k);

    double table[3][3];
    for (int c = 0; c < 3; ++c) {
        std::vector<std::vector<Op>> chunks;
        for (int k = 0; k < CHUNKS; ++k)
            chunks.push_back(load("data/case_" + std::string(1, names[c]) + "_" + std::to_string(k) + ".txt"));

        for (int t = 1; t <= 3; ++t) {
            double sum = 0;
            for (int r = 0; r < REPEATS; ++r) sum += measure(chunks, t);
            table[c][t - 1] = sum / REPEATS;
        }
    }

    std::cout << "\nСередній час виконання, мс (N = " << N << " операцій, "
        << REPEATS << " повторів)\n\n";
    std::cout << "Випадок        1 потік   2 потоки   3 потоки\n";
    std::cout << std::fixed << std::setprecision(2);
    for (int c = 0; c < 3; ++c) {
        std::cout << labels[c];
        for (int t = 0; t < 3; ++t) std::cout << std::setw(11) << table[c][t];
        std::cout << '\n';
    }

    std::cout << "\nСповільнення відносно 1 потоку\n\n";
    std::cout << "Випадок          1 -> 2     1 -> 3\n";
    for (int c = 0; c < 3; ++c) {
        std::cout << labels[c];
        for (int t = 1; t < 3; ++t)
            std::cout << "      x" << std::setw(4) << table[c][t] / table[c][0];
        std::cout << '\n';
    }

    std::cout << "\n(контрольна сума: " << g_sink << ")\n";
}