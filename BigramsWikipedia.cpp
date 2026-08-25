#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;
using Count = std::uint64_t;

// CONFIGURACIÓN DE RENDIMIENTO Y MEMORIA
// 1,000,000 claves por worker
constexpr std::size_t DEFAULT_MAX_KEYS_PER_WORKER = 1000000;
constexpr std::size_t BLOCK_SIZE = 16 * 1024 * 1024; // Bloques de 16 MB para el lector
constexpr std::size_t QUEUE_MULTIPLIER = 2;
constexpr std::size_t TOP_K = 1000;



// UTF-8 & CLASIFICACIÓN UNICODE (\p{L})

bool decodeUtf8(const char*& p, const char* end, std::uint32_t& cp) {
    if (p >= end) return false;
    const unsigned char c = static_cast<unsigned char>(*p);

    if (c < 0x80) { ++p; cp = c; return true; }
    if ((c & 0xE0) == 0xC0) {
        if (p + 1 >= end) return false;
        const unsigned char c1 = static_cast<unsigned char>(p[1]);
        if ((c1 & 0xC0) != 0x80) return false;
        cp = ((c & 0x1F) << 6) | (c1 & 0x3F);
        if (cp < 0x80) return false;
        p += 2; return true;
    }
    if ((c & 0xF0) == 0xE0) {
        if (p + 2 >= end) return false;
        const unsigned char c1 = static_cast<unsigned char>(p[1]);
        const unsigned char c2 = static_cast<unsigned char>(p[2]);
        if ((c1 & 0xC0) != 0x80 || (c2 & 0xC0) != 0x80) return false;
        cp = ((c & 0x0F) << 12) | ((c1 & 0x3F) << 6) | (c2 & 0x3F);
        if (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
        p += 3; return true;
    }
    if ((c & 0xF8) == 0xF0) {
        if (p + 3 >= end) return false;
        const unsigned char c1 = static_cast<unsigned char>(p[1]);
        const unsigned char c2 = static_cast<unsigned char>(p[2]);
        const unsigned char c3 = static_cast<unsigned char>(p[3]);
        if ((c1 & 0xC0) != 0x80 || (c2 & 0xC0) != 0x80 || (c3 & 0xC0) != 0x80) return false;
        cp = ((c & 0x07) << 18) | ((c1 & 0x3F) << 12) | ((c2 & 0x3F) << 6) | (c3 & 0x3F);
        if (cp < 0x10000 || cp > 0x10FFFF) return false;
        p += 4; return true;
    }
    return false;
}

void appendUtf8(std::string& out, std::uint32_t cp) {
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
    }
    else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
    else if (cp <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
    else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

// Clasificación \p{L} con Fast-Path para ASCII
inline bool isUnicodeLetter(std::uint32_t cp, bool& isAsciiOnly) {
    if (cp < 128) {
        return (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z');
    }
    isAsciiOnly = false;
    if (cp <= 0xFFFF) {
        if (cp >= 0xD800 && cp <= 0xDFFF) return false;
        wchar_t wc = static_cast<wchar_t>(cp);
        WORD type = 0;
        if (GetStringTypeExW(LOCALE_INVARIANT, CT_CTYPE1, &wc, 1, &type)) {
            return (type & C1_ALPHA) != 0;
        }
    }
    return false;
}

std::wstring utf8ToWide(const std::string& input) {
    if (input.empty()) return {};
    int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(), static_cast<int>(input.size()), nullptr, 0);
    if (size <= 0) return {};
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(), static_cast<int>(input.size()), result.data(), size) <= 0) return {};
    return result;
}

std::string wideToUtf8(const std::wstring& input) {
    if (input.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input.data(), static_cast<int>(input.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string result(static_cast<std::size_t>(size), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input.data(), static_cast<int>(input.size()), result.data(), size, nullptr, nullptr) <= 0) return {};
    return result;
}

std::wstring toLowerUnicode(const std::wstring& input) {
    if (input.empty()) return {};
    int size = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, input.data(), static_cast<int>(input.size()), nullptr, 0, nullptr, nullptr, 0);
    if (size <= 0) return input;
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    if (LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, input.data(), static_cast<int>(input.size()), result.data(), size, nullptr, nullptr, 0) <= 0) return input;
    while (!result.empty() && result.back() == L'\0') result.pop_back();
    return result;
}

// Normalización rápida (ASCII in-place / Unicode vía API de Windows)
inline std::string normalizeWord(const std::string& word, bool isAsciiOnly) {
    if (isAsciiOnly) {
        std::string res = word;
        for (char& c : res) {
            if (c >= 'A' && c <= 'Z') c += 32;
        }
        return res;
    }
    std::wstring wide = utf8ToWide(word);
    if (wide.empty()) return {};
    wide = toLowerUnicode(wide);
    return wideToUtf8(wide);
}

// E/S BINARIA DE TEMPORALES

void writeU32(std::ofstream& out, std::uint32_t value) { out.write(reinterpret_cast<const char*>(&value), sizeof(value)); }
void writeU64(std::ofstream& out, std::uint64_t value) { out.write(reinterpret_cast<const char*>(&value), sizeof(value)); }
bool readU32(std::ifstream& in, std::uint32_t& value) { return static_cast<bool>(in.read(reinterpret_cast<char*>(&value), sizeof(value))); }
bool readU64(std::ifstream& in, std::uint64_t& value) { return static_cast<bool>(in.read(reinterpret_cast<char*>(&value), sizeof(value))); }

void writeRecord(std::ofstream& out, const std::string& bigram, Count count) {
    const auto length = static_cast<std::uint32_t>(bigram.size());
    writeU32(out, length);
    out.write(bigram.data(), static_cast<std::streamsize>(length));
    writeU64(out, count);
}

bool readRecord(std::ifstream& in, std::string& bigram, Count& count) {
    std::uint32_t length = 0;
    if (!readU32(in, length)) return false;
    bigram.resize(length);
    if (!in.read(bigram.data(), static_cast<std::streamsize>(length))) return false;
    if (!readU64(in, count)) return false;
    return true;
}

fs::path flushToDisk(std::unordered_map<std::string, Count>& localMap, const fs::path& tempDirectory, int workerId, std::uint64_t chunkId) {
    if (localMap.empty()) return {};

    std::vector<std::pair<std::string, Count>> records;
    records.reserve(localMap.size());
    for (const auto& item : localMap) records.push_back(item);

    localMap.clear();
    std::unordered_map<std::string, Count>().swap(localMap); // Liberar memoria RAM del mapa hash

    std::sort(records.begin(), records.end(), [](const auto& a, const auto& b) { return a.first < b.first; });

    const fs::path filename = tempDirectory / ("worker_" + std::to_string(workerId) + "_chunk_" + std::to_string(chunkId) + ".bin");
    std::ofstream out(filename, std::ios::binary);
    if (!out) throw std::runtime_error("No se pudo crear el temporal binario.");

    for (const auto& item : records) writeRecord(out, item.first, item.second);
    out.close();
    return filename;
}

// PARSER DE PALABRAS Y SEPARADORES

void parseLine(const std::string& line, std::unordered_map<std::string, Count>& localMap, std::size_t maxKeys, const std::function<void()>& flush) {
    const char* ptr = line.data();
    const char* end = line.data() + line.size();

    std::string previousWord;
    bool havePrevious = false;
    bool hardBreak = false;

    std::string currentWord;
    std::size_t characterCount = 0;
    bool invalidLongWord = false;
    bool currentIsAscii = true;

    while (ptr < end) {
        const char* charStart = ptr;
        std::uint32_t cp = 0;

        if (!decodeUtf8(ptr, end, cp)) {
            havePrevious = false;
            previousWord.clear();
            hardBreak = true;
            ptr = charStart + 1;
            continue;
        }

        if (cp == '\n') {
            havePrevious = false;
            previousWord.clear();
            hardBreak = false;
            currentWord.clear();
            characterCount = 0;
            invalidLongWord = false;
            currentIsAscii = true;
            continue;
        }

        bool isAsciiChar = true;
        if (isUnicodeLetter(cp, isAsciiChar)) {
            if (!isAsciiChar) currentIsAscii = false;
            if (!invalidLongWord) {
                ++characterCount;
                if (characterCount <= 30) {
                    appendUtf8(currentWord, cp);
                }
                else {
                    invalidLongWord = true;
                }
            }
            continue;
        }

        // Final de la palabra actual
        if (!currentWord.empty() && !invalidLongWord) {
            if (characterCount >= 2 && characterCount <= 30) {
                std::string normalizedWord = normalizeWord(currentWord, currentIsAscii);
                if (!normalizedWord.empty()) {
                    if (havePrevious && !hardBreak) {
                        std::string bigram = previousWord + '\t' + normalizedWord;
                        auto it = localMap.find(bigram);
                        if (it == localMap.end()) {
                            localMap.emplace(std::move(bigram), 1);
                        }
                        else {
                            ++it->second;
                        }
                        if (localMap.size() >= maxKeys) flush();
                    }
                    previousWord = std::move(normalizedWord);
                    havePrevious = true;
                }
                else {
                    havePrevious = false;
                    previousWord.clear();
                }
            }
            else {
                havePrevious = false;
                previousWord.clear();
            }
        }

        currentWord.clear();
        characterCount = 0;
        invalidLongWord = false;
        currentIsAscii = true;

        // Evaluación de separadores
        const bool whitespace = (cp == ' ' || cp == '\t' || cp == '\r' || cp == '\v' || cp == '\f');
        if (!whitespace) {
            havePrevious = false;
            previousWord.clear();
            hardBreak = true; // Símbolo o número rompe la secuencia del bigrama
        }
        else {
            hardBreak = false; // Espacio en blanco solo separa palabras
        }
    }

    if (!currentWord.empty() && !invalidLongWord && characterCount >= 2 && characterCount <= 30) {
        std::string normalizedWord = normalizeWord(currentWord, currentIsAscii);
        if (!normalizedWord.empty() && havePrevious && !hardBreak) {
            std::string bigram = previousWord + '\t' + normalizedWord;
            auto it = localMap.find(bigram);
            if (it == localMap.end()) localMap.emplace(std::move(bigram), 1);
            else ++it->second;
            if (localMap.size() >= maxKeys) flush();
        }
    }
}

// COLA DE TRABAJO Y WORKERS

class BlockingQueue {
private:
    std::queue<std::string> queue;
    std::mutex mutex;
    std::condition_variable notEmpty, notFull;
    std::size_t maxSize;
    bool finished = false;

public:
    explicit BlockingQueue(std::size_t maxQueueSize) : maxSize(maxQueueSize) {}

    void push(std::string item) {
        std::unique_lock<std::mutex> lock(mutex);
        notFull.wait(lock, [&]() { return queue.size() < maxSize; });
        queue.push(std::move(item));
        notEmpty.notify_one();
    }

    bool pop(std::string& item) {
        std::unique_lock<std::mutex> lock(mutex);
        notEmpty.wait(lock, [&]() { return !queue.empty() || finished; });
        if (queue.empty()) return false;
        item = std::move(queue.front());
        queue.pop();
        notFull.notify_one();
        return true;
    }

    void setFinished() {
        { std::lock_guard<std::mutex> lock(mutex); finished = true; }
        notEmpty.notify_all();
    }
};

struct WorkerResult { std::vector<fs::path> chunks; };

void workerFunction(BlockingQueue& queue, int workerId, const fs::path& tempDirectory, std::size_t maxKeys, std::vector<WorkerResult>& results) {
    std::unordered_map<std::string, Count> localMap;
    localMap.reserve(maxKeys);
    std::uint64_t chunkId = 0;

    auto flush = [&]() {
        if (localMap.empty()) return;
        fs::path chunk = flushToDisk(localMap, tempDirectory, workerId, chunkId++);
        if (!chunk.empty()) results[static_cast<std::size_t>(workerId)].chunks.push_back(chunk);
        localMap.reserve(maxKeys);
        };

    std::string block;
    while (queue.pop(block)) {
        std::size_t start = 0;
        while (start < block.size()) {
            std::size_t newline = block.find('\n', start);
            if (newline == std::string::npos) break;
            std::string line = block.substr(start, newline - start);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            parseLine(line, localMap, maxKeys, flush);
            start = newline + 1;
        }
    }
    flush();
}

// K-WAY MERGE & TOP 1000

struct MergeRecord { std::string bigram; Count count; std::size_t fileIndex; };
struct MergeComparator { bool operator()(const MergeRecord& a, const MergeRecord& b) const { return a.bigram > b.bigram; } };

struct TopEntry { std::string bigram; Count count; };
struct TopComparator {
    bool operator()(const TopEntry& a, const TopEntry& b) const {
        if (a.count != b.count) return a.count > b.count;
        return a.bigram < b.bigram;
    }
};

std::vector<TopEntry> mergeChunks(const std::vector<fs::path>& chunkPaths) {
    std::vector<std::ifstream> streams;
    streams.reserve(chunkPaths.size());
    for (const auto& path : chunkPaths) {
        streams.emplace_back(path, std::ios::binary);
        if (!streams.back()) throw std::runtime_error("Error al abrir fragmento binario.");
    }

    std::priority_queue<MergeRecord, std::vector<MergeRecord>, MergeComparator> pq;
    for (std::size_t i = 0; i < streams.size(); ++i) {
        std::string bigram; Count count;
        if (readRecord(streams[i], bigram, count)) pq.push({ std::move(bigram), count, i });
    }

    std::priority_queue<TopEntry, std::vector<TopEntry>, TopComparator> top;
    std::string currentBigram; Count currentCount = 0;

    while (!pq.empty()) {
        MergeRecord record = pq.top();
        pq.pop();

        std::string nextBigram; Count nextCount;
        if (readRecord(streams[record.fileIndex], nextBigram, nextCount)) {
            pq.push({ std::move(nextBigram), nextCount, record.fileIndex });
        }

        if (currentBigram.empty()) {
            currentBigram = std::move(record.bigram);
            currentCount = record.count;
        }
        else if (record.bigram == currentBigram) {
            currentCount += record.count;
        }
        else {
            TopEntry candidate{ currentBigram, currentCount };
            if (top.size() < TOP_K) {
                top.push(std::move(candidate));
            }
            else {
                const TopEntry& worst = top.top();
                if (candidate.count > worst.count || (candidate.count == worst.count && candidate.bigram < worst.bigram)) {
                    top.pop();
                    top.push(std::move(candidate));
                }
            }
            currentBigram = std::move(record.bigram);
            currentCount = record.count;
        }
    }

    if (!currentBigram.empty()) {
        TopEntry candidate{ currentBigram, currentCount };
        if (top.size() < TOP_K) {
            top.push(std::move(candidate));
        }
        else {
            const TopEntry& worst = top.top();
            if (candidate.count > worst.count || (candidate.count == worst.count && candidate.bigram < worst.bigram)) {
                top.pop(); top.push(std::move(candidate));
            }
        }
    }

    std::vector<TopEntry> result;
    result.reserve(top.size());
    while (!top.empty()) { result.push_back(top.top()); top.pop(); }
    std::sort(result.begin(), result.end(), [](const TopEntry& a, const TopEntry& b) {
        if (a.count != b.count) return a.count > b.count;
        return a.bigram < b.bigram;
        });
    return result;
}



int main(int argc, char* argv[]) {
    if (argc < 4 || argc > 5) {
        std::cerr << "USO: BigramasWikipedia.exe \"archivo.txt\" workers \"directorio_salida\" [max_keys]\n";
        return 1;
    }

    try {
        const fs::path inputPath = argv[1];
        const int workers = std::stoi(argv[2]);
        const fs::path outputDirectory = argv[3];
        std::size_t maxKeys = (argc == 5) ? static_cast<std::size_t>(std::stoull(argv[4])) : DEFAULT_MAX_KEYS_PER_WORKER;

        if (workers < 1 || maxKeys < 1000 || !fs::exists(inputPath)) {
            throw std::runtime_error("Parametros invalidos o archivo inexistente.");
        }

        fs::create_directories(outputDirectory);
        const fs::path tempDirectory = outputDirectory / "tmp";
        fs::create_directories(tempDirectory);

        const std::uint64_t fileSize = fs::file_size(inputPath);
        std::cout << "Procesando " << fileSize << " bytes con " << workers << " workers...\n";

        BlockingQueue queue(std::max<std::size_t>(2, static_cast<std::size_t>(workers) * QUEUE_MULTIPLIER));
        std::vector<WorkerResult> results(static_cast<std::size_t>(workers));
        std::vector<std::thread> workerThreads;

        const auto totalStart = std::chrono::steady_clock::now();
        const auto countStart = std::chrono::steady_clock::now();

        for (int i = 0; i < workers; ++i) {
            workerThreads.emplace_back([&queue, &tempDirectory, maxKeys, &results, i]() {
                workerFunction(queue, i, tempDirectory, maxKeys, results);
                });
        }

        std::ifstream input(inputPath, std::ios::binary);
        std::string leftover;
        std::vector<char> buffer(BLOCK_SIZE);

        while (input) {
            input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            std::streamsize readCount = input.gcount();
            if (readCount <= 0) break;

            std::string block;
            block.reserve(leftover.size() + static_cast<std::size_t>(readCount));
            block.append(leftover);
            block.append(buffer.data(), static_cast<std::size_t>(readCount));

            const std::size_t lastNewline = block.rfind('\n');
            if (lastNewline == std::string::npos) {
                leftover = std::move(block);
                continue;
            }

            queue.push(block.substr(0, lastNewline + 1));
            leftover = block.substr(lastNewline + 1);
        }

        if (!leftover.empty()) queue.push(std::move(leftover));
        queue.setFinished();

        for (auto& thread : workerThreads) thread.join();
        input.close();

        const auto countEnd = std::chrono::steady_clock::now();
        const double countSeconds = std::chrono::duration<double>(countEnd - countStart).count();

        std::vector<fs::path> allChunks;
        for (const auto& res : results) {
            for (const auto& chunk : res.chunks) allChunks.push_back(chunk);
        }

        std::cout << "Fase de conteo completada en " << countSeconds << " s. Fragmentos: " << allChunks.size() << "\nIniciando Merge...\n";

        const auto mergeStart = std::chrono::steady_clock::now();
        std::vector<TopEntry> topResults = mergeChunks(allChunks);
        const auto mergeEnd = std::chrono::steady_clock::now();
        const double mergeSeconds = std::chrono::duration<double>(mergeEnd - mergeStart).count();

        // Escritura del archivo final de resultados Top 1000
        const fs::path outputFile = outputDirectory / "top_1000_bigramas.txt";
        std::ofstream output(outputFile);
        output << "rank\tbigrama\tconteo\n";
        for (std::size_t i = 0; i < topResults.size(); ++i) {
            std::string cleanBigram = topResults[i].bigram;
            std::replace(cleanBigram.begin(), cleanBigram.end(), '\t', ' ');
            output << (i + 1) << '\t' << cleanBigram << '\t' << topResults[i].count << '\n';
        }
        output.close();

        const auto totalEnd = std::chrono::steady_clock::now();
        const double totalSeconds = std::chrono::duration<double>(totalEnd - totalStart).count();

        // Generación del archivo con tiempos
        std::ofstream timing(outputDirectory / "timing.txt");
        timing << "workers=" << workers << "\ncount_seconds=" << countSeconds << "\nmerge_seconds=" << mergeSeconds << "\ntotal_seconds=" << totalSeconds << "\n";
        timing.close();

        // Limpieza de temporales binarios
        for (const auto& chunk : allChunks) { std::error_code ec; fs::remove(chunk, ec); }
        { std::error_code ec; fs::remove(tempDirectory, ec); }

        std::cout << "COMPLETADO en " << totalSeconds << " segundos.\nSalida guardada en: " << outputFile.string() << "\n";
        return 0;
    }
    catch (const std::exception& e) {
        std::cerr << "ERROR: " << e.what() << "\n";
        return 1;
    }
}