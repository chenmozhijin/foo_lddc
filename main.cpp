#include "stdafx.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <mutex>
#include "json.hpp"
using json = nlohmann::json;

DECLARE_COMPONENT_VERSION(
    "LDDCDesktopLyrics",
    "0.0.1",
    "Foobar2000 plugin for displaying lyrics on the desktop."
);

VALIDATE_COMPONENT_FILENAME("foo_lddc.dll");


std::wstring string_to_wstring(const std::string& str) {
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), NULL, 0);
    std::wstring wstrTo(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), &wstrTo[0], size_needed);
    return wstrTo;
}


// Helper function to trim \r\n from the end of the string
std::string trimEnd(const std::string& str) {
    std::string trimmed = str;
    while (!trimmed.empty() && (trimmed.back() == '\r' || trimmed.back() == '\n')) {
        trimmed.pop_back();
    }
    return trimmed;
}

bool isValidPort(const std::string& portStr) {
    // Trim \r and \n from the end of the string
    std::string trimmedPortStr = trimEnd(portStr);

    // Check if the string is empty or too long
    if (trimmedPortStr.empty() || trimmedPortStr.length() > 5) {
        return false;
    }

    // Check if all characters are digits
    for (char c : trimmedPortStr) {
        if (!isdigit(c)) {
            return false;
        }
    }

    // Convert the string to an integer
    int port = std::stoi(trimmedPortStr);

    // Check if the integer is within the valid port range
    if (port < 0 || port > 65535) {
        return false;
    }

    return true;
}

double get_unix_time() {
    return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
}

class LDDCDesktopLyricsInitQuit : public initquit {
public:
    LDDCDesktopLyricsInitQuit() : lddc(nullptr), play_callback_impl(nullptr) {}

    void on_init() override {
        lddc = new LDDCDesktopLyrics();
        lddc->initialize();
        play_callback_impl = new play_callback_impl_class(lddc);
        play_callback_manager::get()->register_callback(play_callback_impl, play_callback::flag_on_playback_all, true);
    }

    void on_quit() override {
        play_callback_manager::get()->unregister_callback(play_callback_impl);
        if (lddc) {
            lddc->shutdown();
            delete lddc;
            lddc = nullptr;
        }
        if (play_callback_impl) {
            delete play_callback_impl;
            play_callback_impl = nullptr;
        }
    }

    class LDDCDesktopLyrics {
    public:
        LDDCDesktopLyrics() : sock(INVALID_SOCKET), id(0), version(0), command_thread(nullptr) {}

        void initialize() {
            if (!load_command_line()) {
                MessageBox(NULL, L"Failed to load command line from info.json,Please run the LDDC main program at least once", L"Error", MB_OK | MB_ICONERROR);
                return;
            }
            start_service();
        }

        void shutdown() {
            json j = {
                {"id", id},
                {"task", "del_instance"}
            };

            send_message(j.dump());
            closesocket(sock);
            WSACleanup();
        }

        bool load_command_line() {
            char* localAppData = nullptr;
            size_t len = 0;
            if (_dupenv_s(&localAppData, &len, "LOCALAPPDATA") != 0 || localAppData == nullptr) {
                return false;
            }

            std::string path = std::string(localAppData) + "\\LDDC\\info.json";
            free(localAppData);

            std::ifstream file(path);
            if (!file.is_open()) {
                return false;
            }

            json info = json::parse(file);
            command_line = info["Command Line"];
            return true;
        }

        std::string execute_command(const std::string& cmd) {
            SECURITY_ATTRIBUTES saAttr;
            saAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
            saAttr.bInheritHandle = TRUE;
            saAttr.lpSecurityDescriptor = NULL;

            HANDLE hStdOutRead, hStdOutWrite;
            if (!CreatePipe(&hStdOutRead, &hStdOutWrite, &saAttr, 0)) {
                return "";
            }

            if (!SetHandleInformation(hStdOutRead, HANDLE_FLAG_INHERIT, 0)) {
                CloseHandle(hStdOutRead);
                CloseHandle(hStdOutWrite);
                return "";
            }

            PROCESS_INFORMATION piProcInfo;
            ZeroMemory(&piProcInfo, sizeof(PROCESS_INFORMATION));

            STARTUPINFO siStartInfo;
            ZeroMemory(&siStartInfo, sizeof(STARTUPINFO));
            siStartInfo.cb = sizeof(STARTUPINFO);
            siStartInfo.hStdOutput = hStdOutWrite;
            siStartInfo.hStdError = hStdOutWrite;
            siStartInfo.dwFlags |= STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
            siStartInfo.wShowWindow = SW_HIDE;

            std::string cmdline = "cmd /c " + cmd;
            std::wstring cmdline_w = string_to_wstring(cmdline);

            if (!CreateProcess(NULL, const_cast<LPWSTR>(cmdline_w.c_str()), NULL, NULL, TRUE, 0, NULL, NULL, &siStartInfo, &piProcInfo)) {
                CloseHandle(hStdOutRead);
                CloseHandle(hStdOutWrite);
                return "";
            }

            CloseHandle(hStdOutWrite);

            std::string result;
            DWORD dwRead;
            CHAR chBuf[128];
            BOOL bSuccess = FALSE;
            for (;;) {
                bSuccess = ReadFile(hStdOutRead, chBuf, 128, &dwRead, NULL);
                if (!bSuccess || dwRead == 0) break;
                result.append(chBuf, dwRead);
                if (result.find('\n') != std::string::npos) {
                    if (isValidPort(result)) {
                        result = trimEnd(result);
                        break;
                    }
                    else
                    {
                        result.clear();
                    }

                }
            }

            CloseHandle(piProcInfo.hProcess);
            CloseHandle(piProcInfo.hThread);
            CloseHandle(hStdOutRead);

            return result;
        }

        void start_service() {
            if (command_line.empty()) return;

            std::string cmd = command_line + " --get-service-port";
            std::string result = "";
            try
            {
                result = execute_command(cmd);
            }
            catch (const std::exception&)
            {
                MessageBox(NULL, L"Unable to get LDDC service port", L"Error", MB_OK | MB_ICONERROR);
                return;
            }

            int port = 0;
            try {
                port = std::stoi(result);
            }
            catch (const std::invalid_argument&) {
                std::wstring errorMsg = L"Invalid port number received. Result: " + std::wstring(result.begin(), result.end()) + L"command_line:" + std::wstring(cmd.begin(), cmd.end());
                MessageBox(NULL, errorMsg.c_str(), L"Error", MB_OK | MB_ICONERROR);
                return;
            }
            catch (const std::out_of_range&) {
                MessageBox(NULL, L"Port number out of range.", L"Error", MB_OK | MB_ICONERROR);
                return;
            }
            if (!connect_to_service(port)) {
                MessageBox(NULL, L"Failed to connect to service.", L"Error", MB_OK | MB_ICONERROR);
                return;
            }

            json j_msg = {
                {"task", "new_desktop_lyrics_instance"},
                {"pid", GetCurrentProcessId()},
                {"available_tasks", {"play", "pause", "stop", "prev", "next"}},
                {"info", {
                    {"name", "foo_lddc"},
                    {"repo", "https://github.com/chenmozhijin/foo_lddc"},
                    {"ver", "0.0.1"},
                }}
            };

            send_message(j_msg.dump());

            std::string response_str;
            if (read_message(response_str)) {
                try {
                    json response = json::parse(response_str);
                    version = response["v"];
                    id = response["id"];
                }
                catch (const json::exception) {
                    MessageBox(NULL, L"Failed to parse JSON response.", L"Error", MB_OK | MB_ICONERROR);
                    return;
                }
            }

            if (version < REQUIRED_VERSION) {
                MessageBox(NULL, L"Incompatible version of LDDC service.", L"Error", MB_OK | MB_ICONERROR);
                return;
            }

            command_thread = new std::thread(&LDDCDesktopLyrics::process_commands, this);
        }

        bool connect_to_service(int port) {
            WSADATA wsaData;
            if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
                return false;
            }

            sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (sock == INVALID_SOCKET) {
                WSACleanup();
                return false;
            }

            sockaddr_in server;
            InetPton(AF_INET, L"127.0.0.1", &server.sin_addr.s_addr);
            server.sin_port = htons(port);
            server.sin_family = AF_INET;

            if (connect(sock, (SOCKADDR*)&server, sizeof(server)) == SOCKET_ERROR) {
                closesocket(sock);
                WSACleanup();
                return false;
            }

            return true;
        }

        bool ensure_connected() {
            return sock != INVALID_SOCKET;
        }

        void send_message(const std::string& message) {
            if (!ensure_connected()) {
                return;
            }

            uint32_t length = htonl(static_cast<uint32_t>(message.length()));
            send(sock, reinterpret_cast<const char*>(&length), sizeof(length), 0);
            send(sock, message.c_str(), static_cast<int>(message.length()), 0);
        }

        bool read_message(std::string& response_str) {
            uint32_t length = 0;

            // 读取剩余的消息内容
            if (!msg_buffer.empty()) {
                response_str = msg_buffer;
                msg_buffer.clear();
                return true;
            }

            // 读取消息长度
            int result = recv(sock, reinterpret_cast<char*>(&length), sizeof(length), 0);
            if (result <= 0) return false;

            length = ntohl(length);

            // 读取消息内容
            std::vector<char> buffer(length);
            result = recv(sock, buffer.data(), static_cast<int>(length), 0);
            if (result <= 0) return false;

            response_str.assign(buffer.begin(), buffer.end());

            // 检查是否有剩余的消息内容
            int remaining_length = length - result;
            if (remaining_length > 0) {
                std::vector<char> remaining_buffer(remaining_length);
                result = recv(sock, remaining_buffer.data(), remaining_length, 0);
                if (result > 0) {
                    // 将剩余内容保存到 msg_buffer 中
                    msg_buffer.assign(remaining_buffer.begin(), remaining_buffer.end());
                }
            }

            return true;
        }

        void process_commands() {
            while (true) {
                std::string msg;
                {
                    if (!read_message(msg)) {
                        break;
                    }
                }

                try {
                    json command = json::parse(msg);
                    handle_command(command);
                }
                catch (const json::exception) {
                    MessageBox(NULL, L"Failed to parse command JSON.", L"Error", MB_OK | MB_ICONERROR);
                }
            }
        }

        void handle_command(const json& command) {
            std::string task = command["task"];
            static_api_ptr_t<playback_control> m_playback_control;

            if (task == "play") {
                fb2k::inMainThread([this]() {
                    playback_control::get()->play_or_unpause();
                    });
            }
            else if (task == "pause") {
                fb2k::inMainThread([this]() {
                    playback_control::get()->pause(true);
                    });
            }
            else if (task == "stop") {
                fb2k::inMainThread([this]() {
                    playback_control::get()->stop();
                    });
            }
            else if (task == "prev") {
                fb2k::inMainThread([this]() {
                    playback_control::get()->start(playback_control::track_command_prev, false);
                    });
            }
            else if (task == "next") {
                fb2k::inMainThread([this]() {
                    playback_control::get()->start(playback_control::track_command_next, false);
                    });
            }
        }


        int get_id() const {
            return id;
        }

    private:
        SOCKET sock;
        int id;
        int version;
        std::string command_line;
        std::string msg_buffer; // 用于保存剩余消息的缓冲区
        std::thread* command_thread; // 用于处理命令的线程

        enum {
            REQUIRED_VERSION = 1
        };
    };

    LDDCDesktopLyrics* lddc;

    class play_callback_impl_class : public play_callback {
    public:
        play_callback_impl_class(LDDCDesktopLyrics* lddc) : lddc(lddc) {}

        void on_playback_starting(play_control::t_track_command, bool) override {
            if (!lddc) return;

            json msg = {
                 {"id", lddc->get_id()},
                 {"task", "start"},
                 {"send_time", get_unix_time()},
                 {"playback_time", static_cast<int>(static_cast<double>(playback_control::get()->playback_get_position()) * 1000)}
            };

            lddc->send_message(msg.dump());
        }

        void on_playback_new_track(metadb_handle_ptr p_track) override {
            if (!lddc) return;

            file_info_impl info;
            if (p_track->get_info(info)) {
                json msg = {
                    {"id", lddc->get_id()},
                    {"task", "chang_music"},
                    {"path", p_track->get_path()},
                    {"duration" , static_cast<int>(info.get_length() * 1000)},
                    {"send_time", get_unix_time()},
                    {"playback_time", static_cast<int>(static_cast<double>(playback_control::get()->playback_get_position()) * 1000)}
                };
                if (info.meta_exists("title")) {
                    msg["title"] = info.meta_get("title", 0);
                }
                else {
                    msg["title"] = nullptr;
                }

                if (info.meta_exists("artist")) {
                    msg["artist"] = info.meta_get("artist", 0);
                }
                else {
                    msg["artist"] = nullptr;
                }

                if (info.meta_exists("album")) {
                    msg["album"] = info.meta_get("album", 0);
                }
                else {
                    msg["album"] = nullptr;
                }

                if (info.meta_exists("tracknumber")) {
                    msg["track"] = info.meta_get("tracknumber", 0);
                }
                else {
                    msg["track"] = nullptr;
                }

                lddc->send_message(msg.dump());
            }
        }

        void on_playback_stop(play_control::t_stop_reason p_reason) override {
            if (!lddc) return;

            json msg = {
                 {"id", lddc->get_id()},
                 {"task", "stop"},
                 {"send_time", get_unix_time()},
                 {"playback_time", static_cast<int>(static_cast<double>(playback_control::get()->playback_get_position()) * 1000)}
            };

            lddc->send_message(msg.dump());
        }

        void on_playback_seek(double p_time) override {
            if (!lddc) return;

            json msg = {
                 {"id", lddc->get_id()},
                 {"task", "sync"},
                 {"send_time", get_unix_time()},
                 {"playback_time", static_cast<int>(static_cast<double>(playback_control::get()->playback_get_position()) * 1000)}
            };

            lddc->send_message(msg.dump());
        }

        void on_playback_pause(bool p_state) override {
            if (!lddc) return;

            json msg = {
                 {"id", lddc->get_id()},
                 {"task",p_state ? "pause" : "proceed"},
                 {"send_time", get_unix_time()},
                 {"playback_time", static_cast<int>(static_cast<double>(playback_control::get()->playback_get_position()) * 1000)}
            };

            lddc->send_message(msg.dump());
        }

        void on_playback_edited(metadb_handle_ptr) override {}

        void on_playback_dynamic_info(const file_info&) override {}

        void on_playback_dynamic_info_track(const file_info&) override {}

        void on_playback_time(double p_time) override {
            if (!lddc) return;

            json msg = {
                 {"id", lddc->get_id()},
                 {"task","sync"},
                 {"send_time", get_unix_time()},
                 {"playback_time", static_cast<int>(static_cast<double>(playback_control::get()->playback_get_position()) * 1000)}
            };

            lddc->send_message(msg.dump());
        }

        void on_volume_change(float) override {}

    private:
        LDDCDesktopLyrics* lddc;
    };

    play_callback_impl_class* play_callback_impl;
};

static initquit_factory_t<LDDCDesktopLyricsInitQuit> g_LDDCDesktopLyricsInitQuit;