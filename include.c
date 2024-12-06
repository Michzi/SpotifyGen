#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include <json-c/json.h>
#include <sqlite3.h>
#include <time.h>

#define MAX_TRACKS 100
#define MAX_PLAYLIST_NAME 100
#define SPOTIFY_CLIENT_ID "your_client_id"
#define SPOTIFY_CLIENT_SECRET "your_client_secret"

typedef struct {
    char track_id[50];
    char name[200];
    char artist[200];
    char album[200];
    int popularity;
    int duration_ms;
} Track;

typedef struct {
    char name[MAX_PLAYLIST_NAME];
    Track tracks[MAX_TRACKS];
    int track_count;
} Playlist;

typedef struct {
    char access_token[500];
    time_t expires_at;
} SpotifyAuth;

typedef struct {
    char *buffer;
    size_t size;
} MemoryStruct;

// Function prototypes
SpotifyAuth get_spotify_access_token();
int fetch_recommendations(Playlist *playlist, char *seed_genres, int num_tracks);
int save_playlist_to_spotify(Playlist *playlist, SpotifyAuth *auth);
void generate_music_mood_playlist(char *mood);
int database_init();
void database_close();
int save_playlist_to_database(Playlist *playlist);
int load_playlist_from_database(char *playlist_name, Playlist *playlist);

// Spotify API request handling
static size_t WriteMemoryCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    MemoryStruct *mem = (MemoryStruct *)userp;

    char *ptr = realloc(mem->buffer, mem->size + realsize + 1);
    if (!ptr) {
        printf("Not enough memory (realloc returned NULL)\n");
        return 0;
    }

    mem->buffer = ptr;
    memcpy(&(mem->buffer[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->buffer[mem->size] = 0;

    return realsize;
}

SpotifyAuth get_spotify_access_token() {
    CURL *curl;
    CURLcode res;
    MemoryStruct chunk;
    SpotifyAuth auth = {0};

    chunk.buffer = malloc(1);
    chunk.size = 0;

    curl_global_init(CURL_GLOBAL_ALL);
    curl = curl_easy_init();

    if(curl) {
        char post_data[256];
        snprintf(post_data, sizeof(post_data), 
                 "grant_type=client_credentials&client_id=%s&client_secret=%s", 
                 SPOTIFY_CLIENT_ID, SPOTIFY_CLIENT_SECRET);

        struct curl_slist *headers = NULL;
        headers = curl_slist_append(headers, "Content-Type: application/x-www-form-urlencoded");

        curl_easy_setopt(curl, CURLOPT_URL, "https://accounts.spotify.com/api/token");
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_data);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);

        res = curl_easy_perform(curl);

        if(res != CURLE_OK) {
            fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
        } else {
            json_object *parsed_json = json_tokener_parse(chunk.buffer);
            json_object *access_token_obj;
            json_object *expires_in_obj;

            if (json_object_object_get_ex(parsed_json, "access_token", &access_token_obj) &&
                json_object_object_get_ex(parsed_json, "expires_in", &expires_in_obj)) {
                
                strncpy(auth.access_token, json_object_get_string(access_token_obj), sizeof(auth.access_token));
                int expires_in = json_object_get_int(expires_in_obj);
                auth.expires_at = time(NULL) + expires_in;
            }

            json_object_put(parsed_json);
        }

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
    }

    free(chunk.buffer);
    curl_global_cleanup();

    return auth;
}

int fetch_recommendations(Playlist *playlist, char *seed_genres, int num_tracks) {
    CURL *curl;
    CURLcode res;
    MemoryStruct chunk;
    SpotifyAuth auth = get_spotify_access_token();

    chunk.buffer = malloc(1);
    chunk.size = 0;

    curl_global_init(CURL_GLOBAL_ALL);
    curl = curl_easy_init();

    if(curl) {
        char url[500];
        snprintf(url, sizeof(url), 
                 "https://api.spotify.com/v1/recommendations?seed_genres=%s&limit=%d", 
                 seed_genres, num_tracks);

        struct curl_slist *headers = NULL;
        char auth_header[256];
        snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", auth.access_token);
        headers = curl_slist_append(headers, auth_header);

        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);

        res = curl_easy_perform(curl);

        if(res != CURLE_OK) {
            fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
            return 0;
        } else {
            json_object *parsed_json = json_tokener_parse(chunk.buffer);
            json_object *tracks_array;

            if (json_object_object_get_ex(parsed_json, "tracks", &tracks_array)) {
                int track_count = json_object_array_length(tracks_array);

                for (int i = 0; i < track_count && i < MAX_TRACKS; i++) {
                    json_object *track = json_object_array_get_idx(tracks_array, i);
                    json_object *id, *name, *artists, *album;

                    json_object_object_get_ex(track, "id", &id);
                    json_object_object_get_ex(track, "name", &name);
                    json_object_object_get_ex(track, "artists", &artists);
                    json_object_object_get_ex(track, "album", &album);

                    strncpy(playlist->tracks[i].track_id, json_object_get_string(id), sizeof(playlist->tracks[i].track_id));
                    strncpy(playlist->tracks[i].name, json_object_get_string(name), sizeof(playlist->tracks[i].name));
                    
                    json_object *first_artist = json_object_array_get_idx(artists, 0);
                    json_object *artist_name;
                    json_object_object_get_ex(first_artist, "name", &artist_name);
                    strncpy(playlist->tracks[i].artist, json_object_get_string(artist_name), sizeof(playlist->tracks[i].artist));
                }

                playlist->track_count = track_count;
            }

            json_object_put(parsed_json);
        }

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
    }

    free(chunk.buffer);
    curl_global_cleanup();

    return 1;
}

int save_playlist_to_spotify(Playlist *playlist, SpotifyAuth *auth) {
    // Spotify playlist creation and track addition logic
    return 1;
}

int database_init() {
    sqlite3 *db;
    char *err_msg = 0;
    
    int rc = sqlite3_open("spotify_playlists.db", &db);
    
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Cannot open database: %s\n", sqlite3_errmsg(db));
        sqlite3_close(db);
        return rc;
    }

    char *sql = "CREATE TABLE IF NOT EXISTS playlists("
                "name TEXT PRIMARY KEY,"
                "track_count INTEGER,"
                "created_at DATETIME DEFAULT CURRENT_TIMESTAMP);";

    rc = sqlite3_exec(db, sql, 0, 0, &err_msg);
    
    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQL error: %s\n", err_msg);
        sqlite3_free(err_msg);
        sqlite3_close(db);
        return rc;
    }

    sqlite3_close(db);
    return SQLITE_OK;
}

int save_playlist_to_database(Playlist *playlist) {
    sqlite3 *db;
    char *err_msg = 0;
    int rc = sqlite3_open("spotify_playlists.db", &db);
    
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Cannot open database: %s\n", sqlite3_errmsg(db));
        return rc;
    }

    char sql[1024];
    snprintf(sql, sizeof(sql), 
             "INSERT OR REPLACE INTO playlists (name, track_count) VALUES ('%s', %d);", 
             playlist->name, playlist->track_count);

    rc = sqlite3_exec(db, sql, 0, 0, &err_msg);
    
    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQL error: %s\n", err_msg);
        sqlite3_free(err_msg);
        sqlite3_close(db);
        return rc;
    }

    sqlite3_close(db);
    return SQLITE_OK;
}

void generate_music_mood_playlist(char *mood) {
    Playlist playlist;
    strncpy(playlist.name, mood, sizeof(playlist.name));

    if (fetch_recommendations(&playlist, mood, 20)) {
        database_init();
        save_playlist_to_database(&playlist);
        
        printf("Generated %s Mood Playlist:\n", mood);
        for (int i = 0; i < playlist.track_count; i++) {
            printf("%d. %s - %s\n", i+1, playlist.tracks[i].name, playlist.tracks[i].artist);
        }
    }
}

int main() {
    curl_global_init(CURL_GLOBAL_ALL);

    generate_music_mood_playlist("chill");
    generate_music_mood_playlist("energetic");
    generate_music_mood_playlist("romantic");

    curl_global_cleanup();
    return 0;
}