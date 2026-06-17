#ifndef FILE_UPLOADER_H
#define FILE_UPLOADER_H

#include <Arduino.h>
#include <WiFi.h>
#include <SD.h>

class FileUploader
{
private:
    const char* serverHost;
    int serverPort;
    const char* apiEndpoint;
    const char* bearerToken;

public:
    FileUploader(
        const char* host,
        int port,
        const char* endpoint,
        const char* token
    );

    bool uploadFileToServer(
        String filepath,
        bool sdReady
    );

    void uploadPendingFiles(
        String hiveNo,
        bool sdReady
    );
};

#endif