#include <iostream>
#include <unordered_map>
#include <string>
#include <sstream>
#include <vector>
#include <list>
#include <chrono>
#include <cstring>
#include <fstream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

using namespace std;
using namespace chrono;

// ---- LRU CACHE ----
const int MAX_KEYS = 5;

struct Node {
    string key, value;
};

list<Node> lruList;
unordered_map<string, list<Node>::iterator> lruMap;
unordered_map<string, steady_clock::time_point> expiryMap;

// ---- AOF ----
ofstream aofFile;

void appendToAOF(const vector<string>& tokens) {
    for (int i = 0; i < tokens.size(); i++) {
        aofFile << tokens[i];
        if (i != tokens.size() - 1) aofFile << " ";
    }
    aofFile << "\n";
    aofFile.flush(); // write immediately, don't buffer
}

// ---- EXPIRY CHECK ----
bool isExpired(const string& key) {
    auto it = expiryMap.find(key);
    if (it == expiryMap.end()) return false;
    return steady_clock::now() > it->second;
}

void deleteKey(const string& key) {
    auto it = lruMap.find(key);
    if (it != lruMap.end()) {
        lruList.erase(it->second);
        lruMap.erase(it);
    }
    expiryMap.erase(key);
}

// ---- CORE OPERATIONS ----
void setValue(const string& key, const string& value) {
    if (lruMap.count(key)) {
        lruList.erase(lruMap[key]);
        lruMap.erase(key);
    }

    if ((int)lruList.size() >= MAX_KEYS) {
        auto lru = lruList.back();
        cout << "[EVICT] removing key: " << lru.key << "\n";
        lruMap.erase(lru.key);
        expiryMap.erase(lru.key);
        lruList.pop_back();
    }

    lruList.push_front({key, value});
    lruMap[key] = lruList.begin();
}

string getValue(const string& key) {
    if (isExpired(key)) {
        deleteKey(key);
        return "NULL";
    }

    auto it = lruMap.find(key);
    if (it == lruMap.end()) return "NULL";

    lruList.splice(lruList.begin(), lruList, it->second);
    return it->second->value;
}

// ---- COMMAND HANDLER ----
string handleCommand(const vector<string>& tokens, bool fromAOF = false) {
    if (tokens.empty()) return "ERROR: empty command\n";
    string cmd = tokens[0];

    if (cmd == "SET") {
        if (tokens.size() < 3) return "ERROR: SET requires key and value\n";
        setValue(tokens[1], tokens[2]);
        if (!fromAOF) appendToAOF(tokens); // don't log while replaying
        return "OK\n";
    }

    if (cmd == "GET") {
        if (tokens.size() < 2) return "ERROR: GET requires key\n";
        return getValue(tokens[1]) + "\n";
    }

    if (cmd == "DEL") {
        if (tokens.size() < 2) return "ERROR: DEL requires key\n";
        if (lruMap.count(tokens[1])) {
            deleteKey(tokens[1]);
            if (!fromAOF) appendToAOF(tokens);
            return "1\n";
        }
        return "0\n";
    }

    if (cmd == "EXISTS") {
        if (tokens.size() < 2) return "ERROR: EXISTS requires key\n";
        if (isExpired(tokens[1])) { deleteKey(tokens[1]); return "0\n"; }
        return lruMap.count(tokens[1]) ? "1\n" : "0\n";
    }

    if (cmd == "EXPIRE") {
        if (tokens.size() < 3) return "ERROR: EXPIRE requires key and seconds\n";
        if (!lruMap.count(tokens[1])) return "0\n";
        int seconds = stoi(tokens[2]);
        expiryMap[tokens[1]] = steady_clock::now() + chrono::seconds(seconds);
        if (!fromAOF) appendToAOF(tokens);
        return "1\n";
    }

    if (cmd == "TTL") {
        if (tokens.size() < 2) return "ERROR: TTL requires key\n";
        if (!lruMap.count(tokens[1])) return "-2\n";
        if (!expiryMap.count(tokens[1])) return "-1\n";
        auto remaining = duration_cast<chrono::seconds>(
            expiryMap[tokens[1]] - steady_clock::now()).count();
        return (remaining <= 0 ? "0" : to_string(remaining)) + "\n";
    }

    return "ERROR: unknown command\n";
}

// ---- COMMAND PARSER ----
vector<string> parseCommand(const string& input) {
    vector<string> tokens;
    stringstream ss(input);
    string token;
    while (ss >> token) tokens.push_back(token);
    return tokens;
}

// ---- AOF REPLAY ON STARTUP ----
void replayAOF() {
    ifstream file("appendonly.aof");
    if (!file.is_open()) {
        cout << "[AOF] No existing AOF file, starting fresh\n";
        return;
    }

    cout << "[AOF] Replaying AOF log...\n";
    string line;
    int count = 0;
    while (getline(file, line)) {
        if (line.empty()) continue;
        vector<string> tokens = parseCommand(line);
        handleCommand(tokens, true); // fromAOF = true, don't re-log
        count++;
    }
    cout << "[AOF] Replayed " << count << " commands\n";
}

// ---- TCP SERVER ----
int main() {
    // open AOF file in append mode
    aofFile.open("appendonly.aof", ios::app);
    if (!aofFile.is_open()) {
        cerr << "Failed to open AOF file\n";
        return 1;
    }

    // replay existing data on startup
    replayAOF();

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(6379);

    bind(server_fd, (sockaddr*)&addr, sizeof(addr));
    listen(server_fd, 5);
    cout << "Mini Redis listening on port 6379...\n";

    while (true) {
        int client_fd = accept(server_fd, nullptr, nullptr);
        if (client_fd < 0) continue;
        cout << "Client connected!\n";

        string accumulated = "";

        while (true) {
            char buffer[1024];
            memset(buffer, 0, sizeof(buffer));
            int bytes = recv(client_fd, buffer, sizeof(buffer), 0);
            if (bytes <= 0) break;

            accumulated += string(buffer, bytes);

            size_t pos;
            while ((pos = accumulated.find('\n')) != string::npos) {
                string line = accumulated.substr(0, pos);
                accumulated = accumulated.substr(pos + 1);

                while (!line.empty() && (line.back() == '\r' ||
                       line.back() == ' '))
                    line.pop_back();

                if (line.empty()) continue;

                vector<string> tokens = parseCommand(line);
                string response = handleCommand(tokens);
                send(client_fd, response.c_str(), response.size(), 0);
            }
        }

        cout << "Client disconnected\n";
        close(client_fd);
    }

    close(server_fd);
    aofFile.close();
    return 0;
}
