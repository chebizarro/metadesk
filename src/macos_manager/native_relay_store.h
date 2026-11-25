#import <Foundation/Foundation.h>

NSString *MDRelayConfigDirectory(void);
NSString *MDRelayNormalize(const char *url);
// nil means unreadable/malformed; an absent file is an empty array.
NSArray<NSString *> *MDRelayLoad(NSString *directory);
BOOL MDRelaySave(NSString *directory, NSArray<NSString *> *relays);
