#include "file_handler.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../../utils/config.h"
#include "../../utils/helper.h"
#include "../../utils/structs.h"
#include "../db/db_access.h"
#include "../system/system_access.h"

void handle_file_create(client_t *client, const char *buffer)
{
    struct json_object *parsed_json = json_tokener_parse(buffer);
    struct json_object *payload, *file_name_obj, *file_size_obj,
        *folder_path_obj;
    json_object_object_get_ex(parsed_json, "payload", &payload);
    file_name_obj = json_object_object_get(payload, "fileName");
    file_size_obj = json_object_object_get(payload, "fileSize");
    folder_path_obj = json_object_object_get(payload, "folderPath");

    const char *file_name = json_object_get_string(file_name_obj);
    long file_size = json_object_get_int64(file_size_obj);
    const char *folder_path = json_object_get_string(folder_path_obj);
    char *parent_id = db_get_root_folder_id(client->username);
    char *folder_name = NULL;
    char *folder_path_copy = strdup(folder_path);
    if (folder_path_copy && strlen(folder_path_copy) > 0)
    {
        char *token = strtok((char *)folder_path_copy, "/");
        while (token != NULL)
        {
            folder_name = token;
            parent_id =
                db_get_folder_id(folder_name, client->username, parent_id);
            if (parent_id == NULL)
            {
                send_response(client->socket, 404, "Folder not found");
                json_object_put(parsed_json);
                return;
            }
            token = strtok(NULL, "/");
        }
    }

    char path[MAX_PATH_LENGTH];
    char file_path[MAX_PATH_LENGTH];
    snprintf(path, sizeof(path), "%s/%s/%s", ROOT_FOLDER, client->username,
             folder_path);
    mkdir(path, 0777);

    // Calculate required space
    size_t required_len =
        strlen(path) + strlen(file_name) + 2;  // +2 for '/' and null terminator
    if (required_len > MAX_PATH_LENGTH)
    {
        send_response(client->socket, 400, "Path too long");
        json_object_put(parsed_json);
        return;
    }

    // Safe concatenation
    snprintf(file_path, sizeof(file_path) + 1, "%s/%s", path, file_name);
    int is_file_exists =
        db_check_file_exist(file_name, client->username, parent_id);

    if (is_file_exists)
    {
        send_response(client->socket, 409, "File already exists");
        json_object_put(parsed_json);
        char tmpBuffer[BUFFER_SIZE];
        recv(client->socket, tmpBuffer, BUFFER_SIZE, 0);
        struct json_object *tmp_json = json_tokener_parse(tmpBuffer);
        struct json_object *answer;
        json_object_object_get_ex(tmp_json, "answer", &answer);
        const char *answer_str = json_object_get_string(answer);
        if (strcmp(answer_str, "Y") != 0 && strcmp(answer_str, "y") != 0 &&
            strcmp(answer_str, "N") != 0 && strcmp(answer_str, "n") != 0)
        {
            send_response(client->socket, 400,
                          "Invalid answer. Please respond with Y/N");
            json_object_put(tmp_json);
            json_object_put(parsed_json);
            return;
        }
        if (strcmp(answer_str, "N") == 0 || strcmp(answer_str, "n") == 0)
        {
            json_object_put(tmp_json);
            json_object_put(parsed_json);
            return;
        }

        if (file_exists(file_path))
            remove(file_path);

        json_object_put(tmp_json);
    }

    // Check if file exists, if not create an empty file
    create_empty_file_if_not_exists(file_path);

    // Send success response to client
    send_response(client->socket, 200, "Ready to receive file");

    // Receive and write file
    FILE *fp = fopen(file_path, "wb");
    if (fp != NULL)
    {
        receive_write_file(client->socket, file_size, fp);
        if (is_file_exists)
        {
            char *file_id = db_get_file_id(file_name, parent_id);
            db_delete_file(file_id);
        }
        db_create_file(file_name, file_size, parent_id, client->username);
    }
    else
    {
        send_response(client->socket, 500, "Failed to create file");
    }

    json_object_put(parsed_json);
}

void handle_file_copy(client_t *client, const char *buffer)
{
    struct json_object *parsed_json = json_tokener_parse(buffer);
    struct json_object *payload, *file_path_obj, *to_folder_obj;

    json_object_object_get_ex(parsed_json, "payload", &payload);
    json_object_object_get_ex(payload, "filePath", &file_path_obj);
    json_object_object_get_ex(payload, "toFolder", &to_folder_obj);

    const char *file_path = json_object_get_string(file_path_obj);
    const char *to_folder = json_object_get_string(to_folder_obj);

    char *token = strtok((char *)file_path, "/");
    char *parent_id = db_get_root_folder_id(client->username);
    char *file_id = NULL;
    char *file_name = NULL;

    while (token != NULL)
    {
        file_name = token;
        token = strtok(NULL, "/");
        if (token == NULL)
        {
            file_id = db_get_file_id(file_name, parent_id);
            if (file_id == NULL)
            {
                send_response(client->socket, 404, "File not found");
                json_object_put(parsed_json);
                return;
            }
        }
        else
        {
            parent_id = db_get_folder_id(token, client->username, parent_id);
            if (parent_id == NULL)
            {
                send_response(client->socket, 404, "Folder not found");
                json_object_put(parsed_json);
                return;
            }
        }
    }

    token = strtok((char *)to_folder, "/");
    char *to_folder_id = db_get_root_folder_id(client->username);
    while (token != NULL)
    {
        to_folder_id = db_get_folder_id(token, client->username, to_folder_id);
        if (to_folder_id == NULL)
        {
            send_response(client->socket, 404, "Folder not found");
            json_object_put(parsed_json);
            return;
        }
        token = strtok(NULL, "/");
    }
    if (db_copy_file(file_id, to_folder_id))
    {
        send_response(client->socket, 200, "File copied successfully");
    }
    else
    {
        send_response(client->socket, 500, "Failed to copy file");
    }

    json_object_put(parsed_json);
}

void handle_file_move(client_t *client, const char *buffer)
{
    struct json_object *parsed_json = json_tokener_parse(buffer);
    struct json_object *payload, *file_path_obj, *to_folder_obj;

    json_object_object_get_ex(parsed_json, "payload", &payload);
    json_object_object_get_ex(payload, "filePath", &file_path_obj);
    json_object_object_get_ex(payload, "toFolder", &to_folder_obj);

    const char *file_path = json_object_get_string(file_path_obj);
    const char *to_folder = json_object_get_string(to_folder_obj);

    char *token = strtok((char *)file_path, "/");
    char *parent_id = db_get_root_folder_id(client->username);
    char *file_id = NULL;
    char *file_name = NULL;

    while (token != NULL)
    {
        file_name = token;
        token = strtok(NULL, "/");
        if (token == NULL)
        {
            file_id = db_get_file_id(file_name, parent_id);
            if (file_id == NULL)
            {
                send_response(client->socket, 404, "File not found");
                json_object_put(parsed_json);
                return;
            }
        }
        else
        {
            parent_id = db_get_folder_id(token, client->username, parent_id);
            if (parent_id == NULL)
            {
                send_response(client->socket, 404, "Folder not found");
                json_object_put(parsed_json);
                return;
            }
        }
    }
    token = strtok((char *)to_folder, "/");
    char *to_folder_id = db_get_root_folder_id(client->username);
    while (token != NULL)
    {
        to_folder_id = db_get_folder_id(token, client->username, to_folder_id);
        if (to_folder_id == NULL)
        {
            send_response(client->socket, 404, "Folder not found");
            json_object_put(parsed_json);
            return;
        }
        token = strtok(NULL, "/");
    }

    if (db_move_file(file_id, to_folder_id))
    {
        send_response(client->socket, 200, "File moved successfully");
    }
    else
    {
        send_response(client->socket, 500, "Failed to move file");
    }

    json_object_put(parsed_json);
}

void handle_file_rename(client_t *client, const char *buffer)
{
    struct json_object *parsed_json = json_tokener_parse(buffer);
    struct json_object *payload, *file_path_obj, *new_name_obj;

    json_object_object_get_ex(parsed_json, "payload", &payload);
    json_object_object_get_ex(payload, "filePath", &file_path_obj);
    json_object_object_get_ex(payload, "newFileName", &new_name_obj);

    const char *file_path = json_object_get_string(file_path_obj);
    const char *new_name = json_object_get_string(new_name_obj);

    char *token = strtok((char *)file_path, "/");
    char *parent_id = db_get_root_folder_id(client->username);
    char *file_id = NULL;
    char *file_name = NULL;

    while (token != NULL)
    {
        file_name = token;
        token = strtok(NULL, "/");
        if (token == NULL)
        {
            file_id = db_get_file_id(file_name, parent_id);
            if (file_id == NULL)
            {
                send_response(client->socket, 404, "File not found");
                json_object_put(parsed_json);
                return;
            }
        }
        else
        {
            parent_id = db_get_folder_id(token, client->username, parent_id);
            if (parent_id == NULL)
            {
                send_response(client->socket, 404, "Folder not found");
                json_object_put(parsed_json);
                return;
            }
        }
    }
    if (db_rename_file(file_id, new_name))
    {
        send_response(client->socket, 200, "File renamed successfully");
    }
    else
    {
        send_response(client->socket, 500, "Failed to rename file");
    }

    json_object_put(parsed_json);
}

void handle_file_delete(client_t *client, const char *buffer)
{
    struct json_object *parsed_json = json_tokener_parse(buffer);
    struct json_object *payload, *file_path_obj;

    json_object_object_get_ex(parsed_json, "payload", &payload);
    json_object_object_get_ex(payload, "filePath", &file_path_obj);

    const char *file_path = json_object_get_string(file_path_obj);
    char *token = strtok((char *)file_path, "/");
    char *parent_id = db_get_root_folder_id(client->username);
    char *file_id = NULL;
    char *file_name = NULL;

    while (token != NULL)
    {
        file_name = token;
        token = strtok(NULL, "/");
        if (token == NULL)
        {
            file_id = db_get_file_id(file_name, parent_id);
            if (file_id == NULL)
            {
                send_response(client->socket, 404, "File not found");
                json_object_put(parsed_json);
                return;
            }
        }
        else
        {
            parent_id = db_get_folder_id(token, client->username, parent_id);
            if (parent_id == NULL)
            {
                send_response(client->socket, 404, "Folder not found");
                json_object_put(parsed_json);
                return;
            }
        }
    }
    if (db_delete_file(file_id))
    {
        send_response(client->socket, 200, "File deleted successfully");
    }
    else
    {
        send_response(client->socket, 500, "Failed to delete file");
    }

    json_object_put(parsed_json);
}

void handle_file_set_access(client_t *client, const char *buffer)
{
    struct json_object *parsed_json = json_tokener_parse(buffer);
    struct json_object *payload, *file_id_obj, *access_obj;

    json_object_object_get_ex(parsed_json, "payload", &payload);
    json_object_object_get_ex(payload, "fileId", &file_id_obj);
    json_object_object_get_ex(payload, "access", &access_obj);

    const char *file_id = json_object_get_string(file_id_obj);
    const char *access = json_object_get_string(access_obj);

    if (db_set_file_access(file_id, access))
    {
        send_response(client->socket, 200, "File access updated successfully");
    }
    else
    {
        send_response(client->socket, 500, "Failed to update file access");
    }

    json_object_put(parsed_json);
}

void handle_file_get_access(client_t *client, const char *buffer)
{
    struct json_object *parsed_json = json_tokener_parse(buffer);
    struct json_object *payload, *file_id_obj;

    json_object_object_get_ex(parsed_json, "payload", &payload);
    json_object_object_get_ex(payload, "fileId", &file_id_obj);

    const char *file_id = json_object_get_string(file_id_obj);
    char *access = db_get_file_access(file_id);

    if (access)
    {
        struct json_object *response = json_object_new_object();
        struct json_object *resp_payload = json_object_new_object();

        json_object_object_add(response, "responseCode",
                               json_object_new_int(200));
        json_object_object_add(resp_payload, "access",
                               json_object_new_string(access));
        json_object_object_add(response, "payload", resp_payload);

        const char *response_str = json_object_to_json_string(response);
        send(client->socket, response_str, strlen(response_str), 0);

        json_object_put(response);
        free(access);
    }
    else
    {
        send_response(client->socket, 500, "Failed to get file access");
    }

    json_object_put(parsed_json);
}

void handle_file_search(client_t *client, const char *buffer)
{
    struct json_object *parsed_json = json_tokener_parse(buffer);
    struct json_object *payload, *search_term_obj;

    json_object_object_get_ex(parsed_json, "payload", &payload);
    json_object_object_get_ex(payload, "fileName", &search_term_obj);

    const char *search_term = json_object_get_string(search_term_obj);
    FileList *files = db_search_file(search_term);

    if (files)
    {
        struct json_object *response = json_object_new_object();
        struct json_object *resp_payload = json_object_new_object();
        struct json_object *files_array = json_object_new_array();

        for (int i = 0; i < files->count; i++)
        {
            struct json_object *file = json_object_new_object();
            json_object_object_add(
                file, "fileId",
                json_object_new_string(files->files[i].file_id));
            json_object_object_add(
                file, "fileName",
                json_object_new_string(files->files[i].file_name));
            json_object_object_add(
                file, "fileSize",
                json_object_new_int64(files->files[i].file_size));
            json_object_object_add(
                file, "access", json_object_new_string(files->files[i].access));
            json_object_object_add(
                file, "folderName",
                json_object_new_string(files->files[i].folder_name));
            json_object_object_add(
                file, "createdBy",
                json_object_new_string(files->files[i].created_by));
            json_object_object_add(
                file, "createdAt",
                json_object_new_string(files->files[i].created_at));
            json_object_object_add(
                file, "filePath",
                json_object_new_string(files->files[i].file_path));
            json_object_array_add(files_array, file);
        }

        json_object_object_add(response, "responseCode",
                               json_object_new_int(200));
        json_object_object_add(resp_payload, "files", files_array);
        json_object_object_add(response, "payload", resp_payload);

        const char *response_str = json_object_to_json_string(response);
        send(client->socket, response_str, strlen(response_str), 0);

        json_object_put(response);
        free_file_list(files);
    }
    else
    {
        send_response(client->socket, 500, "Failed to search files");
    }

    json_object_put(parsed_json);
}

void handle_file_download(client_t *client, const char *buffer)
{
    struct json_object *parsed_json = json_tokener_parse(buffer);
    struct json_object *payload, *file_path_obj, *file_owner_obj;

    json_object_object_get_ex(parsed_json, "payload", &payload);
    json_object_object_get_ex(payload, "filePath", &file_path_obj);
    json_object_object_get_ex(payload, "fileOwner", &file_owner_obj);

    const char *file_path = json_object_get_string(file_path_obj);
    const char *file_owner = file_owner_obj
                                 ? json_object_get_string(file_owner_obj)
                                 : client->username;

    char *token = strtok((char *)file_path, "/");
    char *parent_id = db_get_root_folder_id(file_owner);
    char *file_id = NULL;
    char *file_name = NULL;

    while (token != NULL)
    {
        file_name = token;
        token = strtok(NULL, "/");
        if (token == NULL)
        {
            file_id = db_get_file_id(file_name, parent_id);
            if (file_id == NULL)
            {
                send_response(client->socket, 404, "File not found");
                json_object_put(parsed_json);
                return;
            }
            if (strcmp(db_get_file_access(file_id), "download") != 0)
            {
                send_response(client->socket, 403, "File access denied");
                json_object_put(parsed_json);
                return;
            }
        }
        else
        {
            parent_id = db_get_folder_id(token, file_owner, parent_id);
            if (parent_id == NULL)
            {
                send_response(client->socket, 404, "Folder not found");
                json_object_put(parsed_json);
                return;
            }
        }
    }

    // if (parent_id) {
    //     // Send file info and content to client
    //     struct json_object *response = json_object_new_object();
    //     json_object_object_add(response, "responseCode",
    //     json_object_new_int(200)); json_object_object_add(response,
    //     "message", json_object_new_string("File downloaded successfully"));

    //     const char* response_str = json_object_to_json_string(response);
    //     send(client->socket, response_str, strlen(response_str), 0);

    //     json_object_put(response);
    // } else {
    //     send_response(client->socket, 404, "File not found");
    // }

    json_object_put(parsed_json);
}