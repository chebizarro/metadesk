#import "native_relay_store.h"

int main(int argc, char **argv) {
    @autoreleasepool {
        if (argc == 2 && !strcmp(argv[1], "directory")) {
            puts(MDRelayConfigDirectory().fileSystemRepresentation);
            return 0;
        }
        if (argc < 3) return 2;
        if (!strcmp(argv[1], "load")) {
            NSArray<NSString *> *urls = MDRelayLoad(@(argv[2]));
            if (!urls) return 1;
            for (NSString *url in urls) puts(url.UTF8String);
            return 0;
        }
        if (!strcmp(argv[1], "save")) {
            NSMutableArray<NSString *> *urls = [NSMutableArray array];
            for (int i = 3; i < argc; ++i) [urls addObject:@(argv[i])];
            return !MDRelaySave(@(argv[2]), urls);
        }
        return 2;
    }
}
