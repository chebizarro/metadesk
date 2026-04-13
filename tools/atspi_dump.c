/*
 * metadesk — tools/atspi_dump.c
 * M1.6 verification tool: walk and print the AT-SPI2 accessibility tree.
 *
 * Usage: atspi_dump [--json | --compact]
 *
 * Connects to the AT-SPI2 D-Bus bus, walks the full accessibility tree,
 * and prints it in the requested format. Useful for verifying tree
 * coverage and testing serialization.
 */
#include "atspi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void print_node_summary(const MdAtspiNode *node, int depth) {
    if (!node) return;

    for (int i = 0; i < depth; i++) printf("  ");
    printf("[%s] %s: %s",
           node->id ? node->id : "?",
           node->role ? node->role : "unknown",
           node->label ? node->label : "(no label)");

    if (node->state_count > 0 && node->states) {
        printf(" {");
        for (int i = 0; i < node->state_count; i++) {
            if (node->states[i])
                printf("%s%s", i > 0 ? "," : "", node->states[i]);
        }
        printf("}");
    }

    if (node->w > 0 || node->h > 0)
        printf(" @%d,%d %dx%d", node->x, node->y, node->w, node->h);

    printf("\n");

    for (int i = 0; i < node->child_count && node->children; i++)
        print_node_summary(node->children[i], depth + 1);
}

static int count_nodes(const MdAtspiNode *node) {
    if (!node) return 0;
    int count = 1;
    for (int i = 0; i < node->child_count && node->children; i++)
        count += count_nodes(node->children[i]);
    return count;
}

int main(int argc, char **argv) {
    enum { FMT_TREE, FMT_JSON, FMT_COMPACT } format = FMT_TREE;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--json") == 0) format = FMT_JSON;
        else if (strcmp(argv[i], "--compact") == 0) format = FMT_COMPACT;
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            fprintf(stderr, "Usage: %s [--json | --compact]\n", argv[0]);
            return 0;
        }
    }

    fprintf(stderr, "atspi_dump: connecting to AT-SPI2 bus...\n");

    MdAtspiTree *tree = md_atspi_create();
    if (!tree) {
        fprintf(stderr, "ERROR: failed to connect to AT-SPI2\n");
        return 1;
    }

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    MdAtspiNode *root = md_atspi_walk(tree);

    clock_gettime(CLOCK_MONOTONIC, &end);
    double walk_ms = (end.tv_sec - start.tv_sec) * 1000.0 +
                     (end.tv_nsec - start.tv_nsec) / 1000000.0;

    if (!root) {
        fprintf(stderr, "ERROR: failed to walk accessibility tree\n");
        md_atspi_destroy(tree);
        return 1;
    }

    int node_count = count_nodes(root);
    fprintf(stderr, "atspi_dump: walked %d nodes in %.1f ms\n\n",
            node_count, walk_ms);

    switch (format) {
    case FMT_TREE:
        print_node_summary(root, 0);
        break;

    case FMT_JSON: {
        char *json = md_atspi_to_json(root);
        if (json) {
            printf("%s\n", json);
            free(json);
        }
        break;
    }

    case FMT_COMPACT: {
        char *compact = md_atspi_to_compact(root);
        if (compact) {
            printf("%s", compact);
            free(compact);
        }
        break;
    }
    }

    md_atspi_node_free(root);
    md_atspi_destroy(tree);
    return 0;
}
