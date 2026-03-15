#ifndef CONFIG_H
#define CONFIG_H

typedef struct {
    int port;
    int heartbeat_timeout;
    int log_level;
} Config;

Config g_config;

void load_config(const char *filename) {
    g_config.port = 8888;
    g_config.heartbeat_timeout = 9;
    g_config.log_level = 0;

    FILE *f = fopen(filename, "r");
    if (f == NULL) {
        printf("[WARN] 找不到配置文件，使用默认值\n");
        return;
    }
    char key[32], value[32];
    while (fscanf(f, "%31[^=]=%31s\n", key, value) == 2) {
        if (strcmp(key, "port") == 0)
            g_config.port = atoi(value);
        else if (strcmp(key, "heartbeat_timeout") == 0)
            g_config.heartbeat_timeout = atoi(value);
        else if (strcmp(key, "log_level") == 0)
            g_config.log_level = atoi(value);
    }
    fclose(f);
}

#endif
