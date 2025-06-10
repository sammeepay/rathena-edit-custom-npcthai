#ifndef	_STALL_HPP_
#define	_STALL_HPP_

#include "../common/cbasetypes.hpp"
#include "../common/db.hpp"
#include "../common/mmo.hpp"

#include "map.hpp" // struct block_list
#include "status.hpp" // class status_change
#include "unit.hpp" // struct unit_data

#define START_STALL_NUM 90000000
#define START_STALL_UID 0
#define MAX_STALL_SLOT 5
#define BUYINGSTALL_MAX_PRICE 99990000
#define BUYINGSTALL_MAX_AMOUNT 9999
#define STALL_TAX 5 // 5% as KRO (over 10 m zeny)

class map_session_data;
struct block_list;
struct view_data;
class status_change;
struct mail_message;

/// result for close stall ui constants
enum e_stall_result
{
	STALLSTORE_OK                    = 0,  //
	STALLSTORE_POSITION              = 2,  // Not allowed on tte position
	STALLSTORE_LOCATION				 = 3,
	STALLSTORE_OVERWEIGHT            = 8,  // Overweight error
};

struct s_stall_data : public block_list {
	struct view_data vd;
	class status_change sc; //They can't have status changes, but.. they want the visual opt values.

	struct item items_inventory[MAX_STALL_SLOT];
	uint32 price[MAX_STALL_SLOT];
	uint32 itemId[MAX_STALL_SLOT];
	uint16 amount[MAX_STALL_SLOT];

	short type; // 0 vending 1 buying

	int32 vid;
	int32 bid;
	int32 owner_id;
	int32 unique_id, vender_id;
	int32 vend_num;
	int32 timer;
	char message[MESSAGE_SIZE];
	char name[NAME_LENGTH];

	uint32 expire_time;
};

extern std::vector<s_stall_data *> stall_db;
extern std::vector<mail_message> stall_mail_db;
void do_init_stall(void);
void do_final_stall(void);

static int32 stall_getuid(void);
int8 stall_ui_open(map_session_data* sd, uint16 skill_lv, short type);
int8 stall_vending_setup(map_session_data* sd, const char* message, const int16 xPos, const int16 yPos, uint8 *data, int32 count);
int8 stall_buying_setup(map_session_data* sd, const char* message, const int16 xPos, const int16 yPos, const struct STALL_BUYING_SET_sub* itemlist, int32 count, uint64 total_price);
void stall_vending_listreq(map_session_data* sd, int32 id);
void stall_buying_listreq(map_session_data* sd, int32 id);
void stall_vending_purchasereq(map_session_data* sd, int32 aid, int32 uid, const uint8* data, int32 count);
void stall_buying_purchasereq(map_session_data* sd, int32 aid, int32 uid, const struct PACKET_CZ_REQ_TRADE_BUYING_STORE_sub* itemlist, uint32 count );
void stall_remove(struct s_stall_data* st);
void stall_vending_save(struct s_stall_data* st);
void stall_buying_save(struct s_stall_data* st);
void stall_close(map_session_data* sd, int32 uid);
void stall_vending_getbackitems(struct s_stall_data* st);
void stall_buying_getbackzeny(struct s_stall_data* st);
bool stall_isStallOpen(uint32 CID, short type);
bool stall_searchall(map_session_data* sd, const struct s_search_store_search* s, const struct s_stall_data* st, short type);
TIMER_FUNC(stall_timeout);
TIMER_FUNC(stall_init);
TIMER_FUNC(stall_mail_queue);

#endif /* _STALL_HPP_ */
