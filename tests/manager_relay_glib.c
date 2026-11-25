/* Exercise the GTK implementation unchanged, including its real writer. */
#include "relay_store.h"
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "directory")) {
        g_autofree char *directory = md_manager_config_directory();
        puts(directory);
        return 0;
    }
    if (argc < 3) return 2;
    if (!strcmp(argv[1], "load")) {
        g_auto(GStrv) urls = md_manager_relays_load(argv[2], NULL);
        if (!urls) return 1;
        for (size_t i = 0; urls[i]; ++i) puts(urls[i]);
        return 0;
    }
    if (!strcmp(argv[1], "save"))
        return !md_manager_relays_save(argv[2], (const char *const *)(argv + 3), NULL);
    return 2;
}
