#include "FileUploader.h"

FileUploader::FileUploader(
    const char* host,
    int port,
    const char* endpoint,
    const char* token)
{
    serverHost = host;
    serverPort = port;
    apiEndpoint = endpoint;
    bearerToken = token;
}

bool FileUploader::uploadFileToServer(
    String filepath,
    bool sdReady)
{
    if (!sdReady)
        return false;

    if (WiFi.status() != WL_CONNECTED)
        return false;

    if (!filepath.startsWith("/"))
    {
        filepath = "/" + filepath;
    }

    File file = SD.open(filepath, FILE_READ);

    Serial.print("Uploading: ");
    Serial.println(filepath);

    if (!file)
    {
        Serial.println("Failed to open file");
        return false;
    }

    Serial.print("Size: ");
    Serial.println(file.size());

    String filename = filepath;
    filename.replace("/", "");

    String boundary =
        "----ESP32Boundary7MA4YWxkTrZu0gW";

    String head =
        "--" + boundary + "\r\n"
        "Content-Disposition: form-data; "
        "name=\"file\"; filename=\"" +
        filename + "\"\r\n";

    if (filename.endsWith(".wav"))
    {
        head +=
            "Content-Type: audio/wav\r\n\r\n";
    }
    else
    {
        head +=
            "Content-Type: text/csv\r\n\r\n";
    }

    String tail =
        "\r\n--" +
        boundary +
        "--\r\n";

    uint32_t totalLength =
        head.length() +
        file.size() +
        tail.length();

    WiFiClient client;

    if (!client.connect(serverHost, serverPort))
    {
        Serial.println("Connection failed");
        file.close();
        return false;
    }

    client.print("POST ");
    client.print(apiEndpoint);
    client.println(" HTTP/1.1");

    client.print("Host: ");
    client.println(serverHost);

    client.print("Authorization: Bearer ");
    client.println(bearerToken);

    client.println(
        "Content-Type: multipart/form-data; boundary=" +
        boundary);

    client.print("Content-Length: ");
    client.println(totalLength);

    client.println();

    client.print(head);

    uint8_t buffer[1024];

    while (file.available())
    {
        size_t len =
            file.read(
                buffer,
                sizeof(buffer));

        client.write(buffer, len);
    }

    client.print(tail);

    file.close();

    unsigned long start = millis();

    while (!client.available())
    {
        if (millis() - start > 15000)
        {
            Serial.println("Upload timeout");
            client.stop();
            return false;
        }
    }

    String response;

    while (client.available())
    {
        response += client.readString();
    }

    client.stop();

    Serial.println("Server response:");
    Serial.println(response);

    bool success =
        response.indexOf("200 OK") >= 0 ||
        response.indexOf("201 Created") >= 0;

    if (success)
    {
        Serial.print("Deleting uploaded file: ");
        Serial.println(filepath);

        if (SD.remove(filepath))
        {
            Serial.println("Deleted successfully");
        }
        else
        {
            Serial.println("Delete failed");
        }
    }

    return success;
}

void FileUploader::uploadPendingFiles(
    String hiveNo,
    bool sdReady)
{
    if (!sdReady)
        return;

    if (WiFi.status() != WL_CONNECTED)
        return;

    File root = SD.open("/");

    if (!root)
        return;

    File file = root.openNextFile();

    while (file)
    {
        String filename = file.name();

        Serial.print("Found file: ");
        Serial.println(filename);

        bool upload = false;

        if (filename == hiveNo + ".csv")
        {
            upload = true;
        }
        else if (
            filename.startsWith(hiveNo + "_") &&
            filename.endsWith(".wav"))
        {
            upload = true;
        }

        if (upload)
        {
            Serial.println();
            Serial.print("Uploading: ");
            Serial.println(filename);

            uploadFileToServer(
                filename,
                sdReady
            );
        }

        file.close();
        file = root.openNextFile();
    }

    root.close();
}