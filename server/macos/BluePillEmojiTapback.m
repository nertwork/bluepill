#import <Foundation/Foundation.h>
#import <objc/message.h>
#import <objc/runtime.h>
#import <os/log.h>
#include <stdio.h>

static os_log_t bps_log(void) {
    static os_log_t log = nil;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        log = os_log_create("com.bluepill.emojitapback", "helper");
    });
    return log;
}

static void bps_send_reply(NSString *txId, NSString *identifier, NSString *errorMsg) {
    if (!txId || txId.length == 0) {
        return;
    }
    Class ncCls = NSClassFromString(@"NetworkController");
    if (!ncCls) {
        return;
    }
    SEL selShared = NSSelectorFromString(@"sharedInstance");
    if (![ncCls respondsToSelector:selShared]) {
        return;
    }
    id nc = ((id (*)(id, SEL))objc_msgSend)((id)ncCls, selShared);
    if (!nc) {
        return;
    }
    NSMutableDictionary *resp = [NSMutableDictionary dictionary];
    resp[@"transactionId"] = txId;
    if (identifier && identifier.length > 0) {
        resp[@"identifier"] = identifier;
    }
    if (errorMsg && errorMsg.length > 0) {
        resp[@"error"] = errorMsg;
    }
    SEL selSend = NSSelectorFromString(@"sendMessage:");
    if ([nc respondsToSelector:selSend]) {
        ((void (*)(id, SEL, id))objc_msgSend)(nc, selSend, resp);
    }
}

static BOOL bps_is_classic_reaction(NSString *reaction) {
    if (!reaction || reaction.length == 0) {
        return NO;
    }
    NSString *lower = [reaction lowercaseString];
    static NSSet *classicSet = nil;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        classicSet = [NSSet setWithObjects:
            @"love", @"like", @"dislike", @"laugh", @"emphasize", @"question",
            @"-love", @"-like", @"-dislike", @"-laugh", @"-emphasize", @"-question",
            nil];
    });
    return [classicSet containsObject:lower];
}

static NSString *bps_normalize_guid(NSString *rawGuid) {
    if (!rawGuid) {
        return @"";
    }
    NSString *trimmed = [rawGuid stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
    NSRange slash = [trimmed rangeOfString:@"/"];
    if (slash.location != NSNotFound && slash.location + 1 < trimmed.length) {
        return [trimmed substringFromIndex:slash.location + 1];
    }
    return trimmed;
}

static void bps_handle_emoji_reaction(NSDictionary *data, NSString *txId) {
    if (![data isKindOfClass:[NSDictionary class]]) {
        bps_send_reply(txId, nil, @"Invalid reaction payload");
        return;
    }
    NSString *chatGuid = [data[@"chatGuid"] isKindOfClass:[NSString class]] ? data[@"chatGuid"] : @"";
    NSString *rawMsgGuid = [data[@"selectedMessageGuid"] isKindOfClass:[NSString class]] ? data[@"selectedMessageGuid"] : @"";
    NSString *msgGuid = bps_normalize_guid(rawMsgGuid);

    NSString *emoji = [data[@"emoji"] isKindOfClass:[NSString class]] ? data[@"emoji"] : @"";
    NSString *reactionType = [data[@"reactionType"] isKindOfClass:[NSString class]] ? data[@"reactionType"] : @"";
    BOOL isRemoved = NO;
    if ([data[@"isRemoved"] respondsToSelector:@selector(boolValue)]) {
        isRemoved = [data[@"isRemoved"] boolValue];
    }
    if (emoji.length == 0 && reactionType.length > 0) {
        if ([reactionType hasPrefix:@"-"] && reactionType.length > 1) {
            isRemoved = YES;
            emoji = [reactionType substringFromIndex:1];
        } else {
            emoji = reactionType;
        }
    }
    emoji = [emoji stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];

    NSInteger partIndex = 0;
    if ([data[@"partIndex"] respondsToSelector:@selector(integerValue)]) {
        partIndex = [data[@"partIndex"] integerValue];
    }

    if (chatGuid.length == 0 || msgGuid.length == 0 || emoji.length == 0) {
        bps_send_reply(txId, nil, @"Missing chatGuid, selectedMessageGuid, or emoji");
        return;
    }

    dispatch_async(dispatch_get_main_queue(), ^{
        Class regCls = NSClassFromString(@"IMChatRegistry");
        SEL selShared = NSSelectorFromString(@"sharedInstance");
        SEL selExisting = NSSelectorFromString(@"existingChatWithGUID:");
        id registry = (regCls && [regCls respondsToSelector:selShared])
            ? ((id (*)(id, SEL))objc_msgSend)((id)regCls, selShared)
            : nil;
        id chat = (registry && [registry respondsToSelector:selExisting])
            ? ((id (*)(id, SEL, id))objc_msgSend)(registry, selExisting, chatGuid)
            : nil;
        if (!chat) {
            os_log_error(bps_log(), "BLUEPILL_EMOJI: Chat does not exist: %{public}@", chatGuid);
            bps_send_reply(txId, nil, @"Chat does not exist!");
            return;
        }

        Class histCls = NSClassFromString(@"IMChatHistoryController");
        id hist = (histCls && [histCls respondsToSelector:selShared])
            ? ((id (*)(id, SEL))objc_msgSend)((id)histCls, selShared)
            : nil;
        SEL selLoad = NSSelectorFromString(@"loadMessageWithGUID:completionBlock:");
        if (!hist || ![hist respondsToSelector:selLoad]) {
            bps_send_reply(txId, nil, @"IMChatHistoryController unavailable");
            return;
        }

        void (^completion)(id) = ^(id msg) {
            dispatch_async(dispatch_get_main_queue(), ^{
                if (!msg) {
                    os_log_error(bps_log(), "BLUEPILL_EMOJI: Message does not exist: %{public}@", msgGuid);
                    bps_send_reply(txId, nil, @"Message does not exist!");
                    return;
                }

                id partItem = nil;
                SEL selImItem = NSSelectorFromString(@"_imMessageItem");
                SEL selNewItems = NSSelectorFromString(@"_newChatItems");
                if ([msg respondsToSelector:selImItem]) {
                    id item = ((id (*)(id, SEL))objc_msgSend)(msg, selImItem);
                    if (item && [item respondsToSelector:selNewItems]) {
                        id itemsObj = ((id (*)(id, SEL))objc_msgSend)(item, selNewItems);
                        if ([itemsObj isKindOfClass:[NSArray class]]) {
                            NSArray *chatItems = (NSArray *)itemsObj;
                            SEL selAgg = NSSelectorFromString(@"aggregateAttachmentParts");
                            SEL selIdx = NSSelectorFromString(@"index");
                            for (id ci in chatItems) {
                                NSString *cname = NSStringFromClass([ci class]);
                                if ([cname isEqualToString:@"IMAggregateAttachmentMessagePartChatItem"] && [ci respondsToSelector:selAgg]) {
                                    NSArray *agg = ((id (*)(id, SEL))objc_msgSend)(ci, selAgg);
                                    for (id sub in agg) {
                                        if ([sub respondsToSelector:selIdx]) {
                                            NSInteger idx = ((NSInteger (*)(id, SEL))objc_msgSend)(sub, selIdx);
                                            if (idx == partIndex) {
                                                partItem = sub;
                                                break;
                                            }
                                        }
                                    }
                                    if (partItem) break;
                                } else if ([ci respondsToSelector:selIdx]) {
                                    NSInteger idx = ((NSInteger (*)(id, SEL))objc_msgSend)(ci, selIdx);
                                    if (idx == partIndex) {
                                        partItem = ci;
                                        break;
                                    }
                                }
                            }
                        }
                    }
                }

                Class tbCls = NSClassFromString(@"IMEmojiTapback");
                SEL selInitEmoji = NSSelectorFromString(@"initWithEmoji:isRemoved:");
                if (!tbCls || ![tbCls instancesRespondToSelector:selInitEmoji]) {
                    bps_send_reply(txId, nil, @"IMEmojiTapback unavailable");
                    return;
                }
                typedef id (*bps_alloc_fn)(id, SEL) __attribute__((ns_returns_retained));
                typedef id (*bps_init_emoji_fn)(__attribute__((ns_consumed)) id, SEL, id, BOOL) __attribute__((ns_returns_retained));
                typedef id (*bps_init_part_fn)(__attribute__((ns_consumed)) id, SEL, id, id, id) __attribute__((ns_returns_retained));
                typedef id (*bps_init_guid_fn)(__attribute__((ns_consumed)) id, SEL, id, id, id, NSRange, id, id) __attribute__((ns_returns_retained));
                bps_alloc_fn allocFn = (bps_alloc_fn)objc_msgSend;

                id tbAlloc = allocFn((id)tbCls, @selector(alloc));
                id tapback = ((bps_init_emoji_fn)objc_msgSend)(tbAlloc, selInitEmoji, emoji, isRemoved);
                if (!tapback) {
                    bps_send_reply(txId, nil, @"Failed to initialize IMEmojiTapback");
                    return;
                }

                Class senderCls = NSClassFromString(@"IMTapbackSender");
                SEL selInitPart = NSSelectorFromString(@"initWithTapback:chat:messagePartChatItem:");
                SEL selInitGuid = NSSelectorFromString(@"initWithTapback:chat:messageGUID:messagePartRange:messageSummaryInfo:threadIdentifier:");
                id sender = nil;
                if (senderCls && partItem && [senderCls instancesRespondToSelector:selInitPart]) {
                    id sAlloc = allocFn((id)senderCls, @selector(alloc));
                    sender = ((bps_init_part_fn)objc_msgSend)(sAlloc, selInitPart, tapback, chat, partItem);
                } else if (senderCls && [senderCls instancesRespondToSelector:selInitGuid]) {
                    SEL selGuid = NSSelectorFromString(@"guid");
                    SEL selTid = NSSelectorFromString(@"threadIdentifier");
                    SEL selText = NSSelectorFromString(@"text");
                    NSString *resolvedGuid = [msg respondsToSelector:selGuid]
                        ? ((id (*)(id, SEL))objc_msgSend)(msg, selGuid)
                        : msgGuid;
                    NSString *tid = [msg respondsToSelector:selTid]
                        ? ((id (*)(id, SEL))objc_msgSend)(msg, selTid)
                        : nil;
                    id txtObj = [msg respondsToSelector:selText]
                        ? ((id (*)(id, SEL))objc_msgSend)(msg, selText)
                        : nil;
                    NSUInteger len = (txtObj && [txtObj respondsToSelector:@selector(length)])
                        ? ((NSUInteger (*)(id, SEL))objc_msgSend)(txtObj, @selector(length))
                        : 0;
                    id sAlloc = allocFn((id)senderCls, @selector(alloc));
                    sender = ((bps_init_guid_fn)objc_msgSend)(
                        sAlloc, selInitGuid, tapback, chat, resolvedGuid ?: msgGuid, NSMakeRange(0, len), nil, tid
                    );
                }

                SEL selSend = NSSelectorFromString(@"send");
                if (!sender || ![sender respondsToSelector:selSend]) {
                    bps_send_reply(txId, nil, @"Failed to initialize IMTapbackSender");
                    return;
                }

                os_log_info(bps_log(), "BLUEPILL_EMOJI: Sending %{public}@ (removed=%d) to %{public}@ in %{public}@",
                            emoji, isRemoved ? 1 : 0, msgGuid, chatGuid);
                fprintf(stderr, "[BluePillEmojiTapback] Sending %s (removed=%d) to %s in %s\n",
                        emoji.UTF8String ?: "", isRemoved ? 1 : 0, msgGuid.UTF8String ?: "", chatGuid.UTF8String ?: "");
                fflush(stderr);
                ((void (*)(id, SEL))objc_msgSend)(sender, selSend);
                bps_send_reply(txId, msgGuid, nil);
            });
        };

        ((void (*)(id, SEL, id, id))objc_msgSend)(hist, selLoad, msgGuid, [completion copy]);
    });
}

static IMP orig_handleMessage_imp = NULL;

static void bps_swizzled_handleMessage(id self, SEL _cmd, id socket, NSString *rawMessage) {
    if ([rawMessage isKindOfClass:[NSString class]]) {
        NSArray<NSString *> *lines = [rawMessage componentsSeparatedByCharactersInSet:[NSCharacterSet newlineCharacterSet]];
        NSMutableArray<NSString *> *passthrough = [NSMutableArray array];
        BOOL handledAny = NO;

        for (NSString *line in lines) {
            NSString *trimmed = [line stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
            if (trimmed.length == 0) {
                continue;
            }
            NSData *jsonData = [trimmed dataUsingEncoding:NSUTF8StringEncoding];
            NSError *err = nil;
            id obj = jsonData ? [NSJSONSerialization JSONObjectWithData:jsonData options:0 error:&err] : nil;
            if ([obj isKindOfClass:[NSDictionary class]]) {
                NSDictionary *dict = (NSDictionary *)obj;
                NSString *action = [dict[@"action"] isKindOfClass:[NSString class]] ? dict[@"action"] : @"";
                NSDictionary *data = [dict[@"data"] isKindOfClass:[NSDictionary class]] ? dict[@"data"] : nil;
                NSString *txId = [dict[@"transactionId"] isKindOfClass:[NSString class]] ? dict[@"transactionId"] : @"";

                if ([action isEqualToString:@"send-emoji-reaction"]) {
                    handledAny = YES;
                    bps_handle_emoji_reaction(data, txId);
                    continue;
                }
                if ([action isEqualToString:@"send-reaction"] && data) {
                    NSString *rt = [data[@"reactionType"] isKindOfClass:[NSString class]] ? data[@"reactionType"] : @"";
                    if (!bps_is_classic_reaction(rt)) {
                        handledAny = YES;
                        bps_handle_emoji_reaction(data, txId);
                        continue;
                    }
                }
            }
            [passthrough addObject:trimmed];
        }

        if (handledAny) {
            if (passthrough.count > 0 && orig_handleMessage_imp) {
                NSString *remaining = [[passthrough componentsJoinedByString:@"\n"] stringByAppendingString:@"\n"];
                ((void (*)(id, SEL, id, NSString *))orig_handleMessage_imp)(self, _cmd, socket, remaining);
            }
            return;
        }
    }

    if (orig_handleMessage_imp) {
        ((void (*)(id, SEL, id, NSString *))orig_handleMessage_imp)(self, _cmd, socket, rawMessage);
    }
}

static BOOL bps_install_hook(void) {
    static BOOL installed = NO;
    if (installed) {
        return YES;
    }
    Class cls = NSClassFromString(@"BlueBubblesHelper");
    if (!cls) {
        return NO;
    }
    SEL sel = NSSelectorFromString(@"handleMessage:message:");
    Method m = class_getInstanceMethod(cls, sel);
    if (!m) {
        return NO;
    }
    orig_handleMessage_imp = method_getImplementation(m);
    method_setImplementation(m, (IMP)bps_swizzled_handleMessage);
    installed = YES;
    os_log_info(bps_log(), "BLUEPILL_EMOJI: Hooked -[BlueBubblesHelper handleMessage:message:] successfully");
    fprintf(stderr, "[BluePillEmojiTapback] Hooked -[BlueBubblesHelper handleMessage:message:] successfully\n");
    fflush(stderr);
    return YES;
}

@interface BluePillEmojiTapbackLoader : NSObject
@end

@implementation BluePillEmojiTapbackLoader
+ (void)load {
    if (!bps_install_hook()) {
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.5 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
            bps_install_hook();
        });
    }
}
@end
