#import <Foundation/Foundation.h>

typedef NS_ENUM (NSInteger, MEGAChatListItemChangeType) {
    MEGAChatListItemChangeTypeStatus           = 0x01,  // Obsolete
    MEGAChatListItemChangeTypeOwnPrivilege     = 0x02,
    MEGAChatListItemChangeTypeUnreadCount      = 0x04,
    MEGAChatListItemChangeTypeParticipants     = 0x08,
    MEGAChatListItemChangeTypeTitle            = 0x10,
    MEGAChatListItemChangeTypeClosed           = 0x20,
    MEGAChatListItemChangeTypeLastMsg          = 0x40,
    MEGAChatListItemChangeTypeLastTs           = 0x80,
    MEGAChatListItemChangeTypeArchived         = 0x100,
    MEGAChatListItemChangeTypeCall             = 0x200,
    MEGAChatListItemChangeTypeChatMode         = 0x400,
    MEGAChatListItemChangeTypeUpdatePreviewers = 0x800,
    MEGAChatListItemChangeTypePreviewClosed    = 0x1000
};

typedef NS_ENUM (NSInteger, MEGAChatMessageType);
typedef NS_ENUM (NSInteger, MEGAChatRoomPrivilege);

@interface MEGAChatListItem : NSObject

@property (readonly, nonatomic) uint64_t chatId;
@property (readonly, nonatomic) NSString *title;
@property (readonly, nonatomic) MEGAChatListItemChangeType changes;
@property (readonly, nonatomic) MEGAChatRoomPrivilege ownPrivilege;
@property (readonly, nonatomic) NSInteger unreadCount;
@property (readonly, nonatomic, getter=isGroup) BOOL group;
@property (readonly, nonatomic, getter=isPublicChat) BOOL publicChat;
@property (readonly, nonatomic, getter=isPreview) BOOL preview;
@property (readonly, nonatomic) uint64_t peerHandle;
@property (readonly, nonatomic, getter=isActive) BOOL active;
@property (readonly, nonatomic) NSUInteger previewersCount;

@property (readonly, nonatomic) NSString *lastMessage;
@property (readonly, nonatomic) uint64_t lastMessageId;
@property (readonly, nonatomic) MEGAChatMessageType lastMessageType;
@property (readonly, nonatomic) uint64_t lastMessageSender;
@property (readonly, nonatomic) NSDate *lastMessageDate;
@property (readonly, nonatomic) MEGAChatMessageType lastMessagePriv;
@property (readonly, nonatomic) uint64_t lastMessageHandle;
@property (strong, nonatomic) NSString *searchString;

- (instancetype)clone;

- (BOOL)hasChangedForType:(MEGAChatListItemChangeType)changeType;

+ (NSString *)stringForChangeType:(MEGAChatListItemChangeType)changeType;
+ (NSString *)stringForOwnPrivilege:(MEGAChatRoomPrivilege)ownPrivilege;
+ (NSString *)stringForMessageType:(MEGAChatMessageType)type;

@end
