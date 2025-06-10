#include "stall.hpp"

#include <stdlib.h> // atoi
#include <sstream>

#include "../common/malloc.hpp" // aMalloc, aFree
#include "../common/nullpo.hpp"
#include "../common/showmsg.hpp" // ShowInfo
#include "../common/strlib.hpp"
#include "../common/timer.hpp"  // DIFF_TICK

#include "achievement.hpp"
#include "battle.hpp"
#include "chrif.hpp"
#include "clif.hpp"
#include "intif.hpp" //mail send
#include "itemdb.hpp"
#include "log.hpp"
#include "npc.hpp"
#include "pc.hpp"
#include "pc_groups.hpp"
#include "vending.hpp"

//Stall
static int32 stall_id = START_STALL_NUM;
static int32 stall_uid = START_STALL_UID;
std::vector<s_stall_data *> stall_db;
std::vector<mail_message> stall_mail_db;

/// failure constants for clif functions
enum e_buyingstore_failure
{
	BUYINGSTORE_CREATE               = 1,  // "Failed to open buying store."
	BUYINGSTORE_CREATE_OVERWEIGHT    = 2,  // "Total amount of then possessed items exceeds the weight limit by %d. Please re-enter."
	BUYINGSTORE_TRADE_BUYER_ZENY     = 3,  // "All items within the buy limit were purchased."
	BUYINGSTORE_TRADE_BUYER_NO_ITEMS = 4,  // "All items were purchased."
	BUYINGSTORE_TRADE_SELLER_FAILED  = 5,  // "The deal has failed."
	BUYINGSTORE_TRADE_SELLER_COUNT   = 6,  // "The trade failed, because the entered amount of item %s is higher, than the buyer is willing to buy."
	BUYINGSTORE_TRADE_SELLER_ZENY    = 7,  // "The trade failed, because the buyer is lacking required balance."
	BUYINGSTORE_CREATE_NO_INFO       = 8,  // "No sale (purchase) information available."
};

static const t_itemid buyingstore_blankslots[MAX_SLOTS] = { 0 };

/**
 Create an unique vending shop id.
 @return the next vending_id
*/
static int32 stall_getid(void)
{
	if( stall_id >= START_STALL_NUM && !map_blid_exists(stall_id) )
		return stall_id++;// available
	else {// find next id
		int32 base_id = stall_id;
		while( base_id != ++stall_id ) {
			if( stall_id < START_STALL_NUM )
				stall_id = START_STALL_NUM;
			if( !map_blid_exists(stall_id) )
				return stall_id++;// available
		}
		// full loop, nothing available
		ShowFatalError("stall_get_new_stall_id: All ids are taken. Exiting...");
		exit(1);
	}
}

static int32 stall_getuid(void)
{
	if( stall_uid >= START_STALL_UID && !map_blid_exists(stall_uid) )
		return stall_uid++;// available
	else {// find next id
		int32 base_id = stall_uid;
		while( base_id != ++stall_uid) {
			if(stall_uid < START_STALL_UID)
				stall_uid = START_STALL_UID;
			if( !map_blid_exists(stall_id) )
				return stall_uid++;// available
		}
		// full loop, nothing available
		ShowFatalError("stall_get_new_stall_uid: All uids are taken. Exiting...");
		exit(1);
	}
}

/**
* Open stall UI for vendor
* @param sd Player
* @param skill_lv level of skill used
* @param type 0 = vending - 1 = buying
*/
int8 stall_ui_open(map_session_data* sd, uint16 skill_lv, short type){
	nullpo_retr(1, sd);

	if (sd->state.vending || sd->state.buyingstore || sd->state.trading) {
		return 1;
	}

	if( sd->sc.getSCE(SC_NOCHAT) && (sd->sc.getSCE(SC_NOCHAT)->val1&MANNER_NOROOM) )
	{// custom: mute limitation
		return 2;
	}

	if( map_getmapflag(sd->m, MF_NOVENDING) )
	{// custom: no vending maps
		clif_displaymessage(sd->fd, msg_txt(sd,276)); // "You can't open a shop on this map"
		return 3;
	}

	if(type == 0)
		clif_stall_vending_open(sd);
	else
		clif_stall_buying_open(sd);

	return 0;
}

// Helper function to check for a player at the specified location
static int32 check_player_at_location(struct block_list* bl, va_list ap) {
    if (bl->type == BL_PC || bl->type == BL_STALL) {
        return 1; // Found a player at the location
    }
    return 0; // No player found, continue searching
}

/**
* Player setup a new vending stall
* @param sd : player opening the shop
* @param message : shop title
* @param xPos : pos X
* @param yPos : pos Y
* @param data : itemlist data
*	data := {<index>.w <amount>.w <value>.l}[count]
* @param count : number of different items
* @return 0 If success, 1 - Cannot open (die, not state.prevend, trading), 2 - No cart, 3 - Count issue, 4 - Cart data isn't saved yet, 5 - No valid item found
*/
int8 stall_vending_setup(map_session_data* sd, const char* message, const int16 xPos, const int16 yPos, uint8 *data, int32 count)
{
	int32 i, j, k, l;
	char message_sql[MESSAGE_SIZE*2];
	StringBuf buf;
	struct block_list npc_near_bl;

	nullpo_retr(false,sd);

	if ( pc_isdead(sd) || !sd->state.prevend || pc_istrading(sd) ) { //add check existing stall
		clif_stall_ui_close(sd,100,STALLSTORE_OK);
		return 1; // can't open stalls lying dead || didn't use via the skill (wpe/hack) || can't have 2 shops at once
	}

	// check number of items in shop
	if( count < 1 || count > 2 + sd->stallvending_level ) { // invalid item count
		clif_skill_fail(*sd, ALL_ASSISTANT_VENDING, USESKILL_FAIL_LEVEL, 0);
		clif_stall_ui_close(sd,100,STALLSTORE_OK);
		return 3;
	}

	// check if shop is allow on the cell
	if( map_getcell(sd->m,xPos,yPos,CELL_CHKNOVENDING) ) {
		clif_stall_ui_close(sd,100,STALLSTORE_POSITION);
		return 1;
	}
	
	// Check if the cell is walkable before setting up the stall
	if (!map_getcell(sd->m, xPos, yPos, CELL_CHKPASS)) {
		clif_stall_ui_close(sd,100,STALLSTORE_POSITION);
		return 1;
	}

	if (map_foreachincell(check_player_at_location, sd->m, xPos, yPos, BL_PC)) {
        clif_stall_ui_close(sd,101,STALLSTORE_LOCATION);
		return 1;
	}

	if (map_foreachincell(check_player_at_location, sd->m, xPos, yPos, BL_STALL)) {
        clif_stall_ui_close(sd,101,STALLSTORE_LOCATION);
		return 1;
	}

	npc_near_bl.m = sd->m;
	npc_near_bl.x = xPos;
	npc_near_bl.y = yPos;

	if( npc_isnear(&npc_near_bl) ) {
		clif_stall_ui_close(sd,100,STALLSTORE_LOCATION);
		return 1;
	}

	struct s_stall_data *st = (struct s_stall_data*)aCalloc(1, sizeof(struct s_stall_data));
	st->owner_id = sd->status.char_id; // Got it now to send items back in case something wrong

	if (save_settings&CHARSAVE_VENDING) // Avoid invalid data from saving
		chrif_save(sd, CSAVE_INVENTORY);

	// filter out invalid items
	i = 0;
	for( j = 0; j < count; j++ ) {
		short index        = *(uint16*)(data + 8*j + 0);
		short amount       = *(uint16*)(data + 8*j + 2);
		uint32 value = *(uint32*)(data + 8*j + 4);

		index = index - 2; // TODO: clif::server_index

		if( index < 0 || index >= MAX_INVENTORY // invalid position
		||  sd->inventory.u.items_inventory[index].amount < 0 // invalid item or insufficient quantity
		//NOTE: official server does not do any of the following checks!
		||  !sd->inventory.u.items_inventory[index].identify // unidentified item
		||  sd->inventory.u.items_inventory[index].attribute == 1 // broken item
		||  sd->inventory.u.items_inventory[index].expire_time // It should not be in the cart but just in case
		||  (sd->inventory.u.items_inventory[index].bound && !pc_can_give_bounded_items(sd)) // can't trade account bound items and has no permission
		||  !itemdb_cantrade(&sd->inventory.u.items_inventory[index], pc_get_group_level(sd), pc_get_group_level(sd)) ) // untradeable item
			continue;

		memcpy(&st->items_inventory[i],&sd->inventory.u.items_inventory[index],sizeof(struct item));

		pc_delitem(sd, index, amount, 0, 0, LOG_TYPE_VENDING);

		st->items_inventory[i].amount = amount;
		st->price[i] = min(value, (uint32)battle_config.vending_max_value);
		i++; // item successfully added
	}

	if (i != j || j > MAX_STALL_SLOT) {
		clif_displaymessage(sd->fd, msg_txt(sd, 266)); //"Some of your items cannot be vended and were removed from the shop."
		clif_skill_fail(*sd, ALL_ASSISTANT_VENDING, USESKILL_FAIL_LEVEL, 0); // custom reply packet
		clif_stall_ui_close(sd,100,STALLSTORE_OK);
		stall_vending_getbackitems(st);
		aFree(st);
		return 5;
	}

	if( i == 0 ) { // no valid item found
		clif_skill_fail(*sd, ALL_ASSISTANT_VENDING, USESKILL_FAIL_LEVEL, 0); // custom reply packet
		clif_stall_ui_close(sd,100,STALLSTORE_OK);
		aFree(st);
		return 5;
	}

	st->vid = sd->id;
	st->type = 0; // TODO vending
	st->vender_id = stall_getid();
	st->unique_id = stall_getuid();
	st->vend_num = i;
	st->expire_time = sd->stall_expire_time;
	safestrncpy(st->message, message, MESSAGE_SIZE);
	safestrncpy(st->name, sd->status.name, NAME_LENGTH);

	st->id = st->vender_id;
	st->type = BL_STALL;
	st->m = sd->m;
	st->x = xPos;
	st->y = yPos;

	st->vd.look[LOOK_BASE] = sd->vd.look[LOOK_BASE];
	st->vd.look[LOOK_WEAPON] = sd->vd.look[LOOK_WEAPON];
	st->vd.look[LOOK_SHIELD] = sd->vd.look[LOOK_SHIELD];
	st->vd.look[LOOK_HEAD_TOP] = sd->vd.look[LOOK_HEAD_TOP];
	st->vd.look[LOOK_HEAD_MID] = sd->vd.look[LOOK_HEAD_MID];
	st->vd.look[LOOK_HEAD_BOTTOM] = sd->vd.look[LOOK_HEAD_BOTTOM];
	st->vd.look[LOOK_HAIR] = sd->vd.look[LOOK_HAIR];
	st->vd.look[LOOK_HAIR_COLOR] = sd->vd.look[LOOK_HAIR_COLOR];
	st->vd.look[LOOK_CLOTHES_COLOR] = sd->vd.look[LOOK_CLOTHES_COLOR];
	st->vd.look[LOOK_BODY2] = sd->vd.look[LOOK_BODY2];
	st->vd.sex = sd->vd.sex;
	st->vd.look[LOOK_ROBE] = sd->vd.look[LOOK_ROBE];

	Sql_EscapeString( mmysql_handle, message_sql, st->message );

	if( Sql_Query( mmysql_handle, "INSERT INTO `%s`(`id`, `uid`, `char_id`, `type`, `class`, `sex`, `map`, `x`, `y`,"
								  "`title`, `hair`, `hair_color`, `body`, `weapon`, `shield`, `head_top`, `head_mid`, `head_bottom`, `robe`,"
								  "`clothes_color`, `name`, `expire_time`) "
		"VALUES( %d, %d, %d, %d, %d, '%c', '%s', %d, %d, '%s', %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, '%s', %u  );",
		stalls_table, st->vender_id, st->unique_id, st->owner_id, st->type, st->vd.look[LOOK_BASE], st->vd.sex == SEX_FEMALE ? 'F' : 'M', mapindex_id2name(st->m), st->x, st->y,
		message_sql, st->vd.look[LOOK_HAIR], st->vd.look[LOOK_HAIR_COLOR], st->vd.look[LOOK_BODY2], st->vd.look[LOOK_WEAPON], st->vd.look[LOOK_SHIELD], st->vd.look[LOOK_HEAD_TOP], st->vd.look[LOOK_HEAD_MID], st->vd.look[LOOK_HEAD_BOTTOM], st->vd.look[LOOK_ROBE],
		st->vd.look[LOOK_CLOTHES_COLOR], st->name, st->expire_time) != SQL_SUCCESS ) {
		Sql_ShowDebug(mmysql_handle);
	}

	StringBuf_Init(&buf);
	StringBuf_Printf(&buf, "INSERT INTO `%s`(`stalls_id`,`index`,`nameid`,`amount`,`identify`,`refine`,`attribute`",stalls_vending_items_table);
	for( l = 0; l < MAX_SLOTS; ++l )
		StringBuf_Printf(&buf, ", `card%d`", l);
	for( l = 0; l < MAX_ITEM_RDM_OPT; ++l ) {
		StringBuf_Printf(&buf, ", `option_id%d`", l);
		StringBuf_Printf(&buf, ", `option_val%d`", l);
		StringBuf_Printf(&buf, ", `option_parm%d`", l);
	}
	StringBuf_Printf(&buf, ",`expire_time`,`bound`,`unique_id`,`enchantgrade`,`price`) VALUES", stalls_vending_items_table);
	for (j = 0; j < i; j++) {
		StringBuf_Printf(&buf, "(%d, %d, %u, %d, %d, %d, %d",
			st->vender_id, j, st->items_inventory[j].nameid, st->items_inventory[j].amount, st->items_inventory[j].identify, st->items_inventory[j].refine, st->items_inventory[j].attribute);
		for( k = 0; k < MAX_SLOTS; ++k )
			StringBuf_Printf(&buf, ", %u", st->items_inventory[j].card[k]);
		for( k = 0; k < MAX_ITEM_RDM_OPT; ++k ) {
			StringBuf_Printf(&buf, ", %d", st->items_inventory[j].option[k].id);
			StringBuf_Printf(&buf, ", %d", st->items_inventory[j].option[k].value);
			StringBuf_Printf(&buf, ", %d", st->items_inventory[j].option[k].param);
		}
		StringBuf_Printf(&buf, ", %u, %d , '%" PRIu64 "', %d, %d)",
			st->items_inventory[j].expire_time, st->items_inventory[j].bound, st->items_inventory[j].unique_id, st->items_inventory[j].enchantgrade, st->price[j]);
		if (j < i-1)
			StringBuf_AppendStr(&buf, ",");
	}
	if (SQL_ERROR == Sql_QueryStr(mmysql_handle, StringBuf_Value(&buf)))
		Sql_ShowDebug(mmysql_handle);
	StringBuf_Destroy(&buf);

	st->timer = add_timer(gettick() + (st->expire_time - time(nullptr)) * 1000,
				stall_timeout, st->id, 0);

	clif_stall_showunit(sd,st);
	clif_showstallboard(sd,st->vender_id,st->message);
	clif_stall_ui_close(sd,100,STALLSTORE_OK);

	if(map_addblock(st))
		return -1;
	status_change_init(st);
	map_addiddb(st);
	stall_db.push_back(st);

	return 0;
}

/**
* Player setup a new vending stall
* @param sd : player opening the shop
* @param message : shop title
* @param xPos : pos X
* @param yPos : pos Y
* @param data : itemlist data
*	data := {<index>.w <amount>.w <value>.l}[count]
* @param count : number of different items
* @return 0 If success, 1 - Cannot open (die, not state.prevend, trading), 2 - No cart, 3 - Count issue, 4 - Cart data isn't saved yet, 5 - No valid item found
*/
#if PACKETVER >= 20170208
int8 stall_buying_setup(map_session_data* sd, const char* message, const int16 xPos, const int16 yPos, const struct STALL_BUYING_SET_sub* itemlist, int32 count, uint64 total_price)
{
	int32 i, j, weight, listidx;
	char message_sql[MESSAGE_SIZE*2];
	StringBuf buf;
	struct block_list npc_near_bl;

	nullpo_retr(false,sd);

	if ( pc_isdead(sd) || !sd->state.prevend || pc_istrading(sd) ) { //add check existing stall
		clif_stall_ui_close(sd,101,STALLSTORE_OK);
		return 1; // can't open stalls lying dead || didn't use via the skill (wpe/hack) || can't have 2 shops at once
	}

	// check number of items in shop
	if( count < 1 || count > 2 + sd->stallvending_level ) { // invalid item count
		clif_stall_ui_close(sd,101,STALLSTORE_OK);
		return 3;
	}
	
	// Check if the cell is walkable before setting up the stall
	if (!map_getcell(sd->m, xPos, yPos, CELL_CHKPASS)) {
		clif_stall_ui_close(sd,101,STALLSTORE_POSITION);
		return 1;
	}

	// check if shop is allow on the cell
	if( map_getcell(sd->m,xPos,yPos,CELL_CHKNOVENDING) ) {
		clif_stall_ui_close(sd,101,STALLSTORE_LOCATION);
		return 1;
	}

	if (map_foreachincell(check_player_at_location, sd->m, xPos, yPos, BL_PC)) {
        clif_stall_ui_close(sd,101,STALLSTORE_LOCATION);
		return 1;
	}

	if (map_foreachincell(check_player_at_location, sd->m, xPos, yPos, BL_STALL)) {
        clif_stall_ui_close(sd,101,STALLSTORE_LOCATION);
		return 1;
	}

	if(total_price <= 0){
		clif_displaymessage(sd->fd, "Buying prices can't be 0.");
		clif_stall_ui_close(sd,101,STALLSTORE_OK);
		return 1;
	}

	npc_near_bl.m = sd->m;
	npc_near_bl.x = xPos;
	npc_near_bl.y = yPos;

	if( npc_isnear(&npc_near_bl) ) {
		clif_stall_ui_close(sd,101,STALLSTORE_LOCATION);
		return 1;
	}

	struct s_stall_data *st = (struct s_stall_data*)aCalloc(1, sizeof(struct s_stall_data));
	st->owner_id = sd->status.char_id; // Got it now to send items back in case something wrong

	if (save_settings&CHARSAVE_VENDING) // Avoid invalid data from saving
		chrif_save(sd, CSAVE_INVENTORY);

	weight = sd->weight;

	// filter out invalid items
	i = 0;
	uint32 temp_price = 0;
	for( j = 0; j < count; j++ ) {
		const struct STALL_BUYING_SET_sub *item = &itemlist[i];
		std::shared_ptr<item_data> id = item_db.find(item->itemId);

		if( id == nullptr || item->count == 0 // invalid input
		||  item->price <= 0 || item->price > BUYINGSTALL_MAX_PRICE // invalid price: unlike vending, items cannot be bought at 0 Zeny
		||	!id->flag.buyingstore || !itemdb_cantrade_sub( id.get(), pc_get_group_level( sd ), pc_get_group_level( sd ) ) ) // untradeable item
			continue;

		int32 idx = pc_search_inventory( sd, item->itemId );

		// At least one must be owned
		if( idx < 0 ){
			break;
		}

		// too many items of same kind
		if( sd->inventory.u.items_inventory[idx].amount + item->count > BUYINGSTALL_MAX_AMOUNT ){
			break;
		}

		// duplicate check. as the client does this too, only malicious intent should be caught here
		if( j ){
			ARR_FIND( 0, j, listidx, sd->buyingstore.items[listidx].nameid == item->itemId );

			// duplicate
			if( listidx != j ){
				ShowWarning( "stall_buying_setup: Found duplicate item on buying list (nameid=%u, amount=%hu, account_id=%d, char_id=%d).\n", item->itemId, item->count, sd->status.account_id, sd->status.char_id );
				break;
			}
		}

		weight+= id->weight*item->count;
		st->itemId[i] = item->itemId;
		st->amount[i] = item->count;
		st->price[i]  = item->price;

		uint32 price = item->count * item->price;
		if(item->price > 10000000)
			price = price * (100 + STALL_TAX) / 100;
		temp_price += price;

		i++; // item successfully added
	}

	if (i != j || j > MAX_STALL_SLOT) {
		clif_displaymessage(sd->fd, msg_txt(sd, 266)); //"Some of your items cannot be vended and were removed from the shop."
		clif_skill_fail(*sd, ALL_ASSISTANT_BUYING, USESKILL_FAIL_LEVEL, 0); // custom reply packet
		clif_stall_ui_close(sd,101,STALLSTORE_OK);
		stall_buying_getbackzeny(st);
		aFree(st);
		return 5;
	}

	if( (sd->max_weight*90)/100 < weight )
	{// not able to carry all wanted items without getting overweight (90%)
		clif_stall_ui_close(sd,101,STALLSTORE_OVERWEIGHT);
		stall_buying_getbackzeny(st);
		aFree(st);
		return 7;
	}

	if( i == 0 ) { // no valid item found
		clif_skill_fail(*sd, ALL_ASSISTANT_BUYING, USESKILL_FAIL_LEVEL, 0); // custom reply packet
		clif_stall_ui_close(sd,101,STALLSTORE_OK);
		aFree(st);
		return 5;
	}

	pc_payzeny(sd, temp_price, LOG_TYPE_BUYING_STORE);
	st->bid = sd->id;
	st->type = 1;
	st->vender_id = stall_getid();
	st->unique_id = stall_getuid();
	st->vend_num = i;
	st->expire_time = sd->stall_expire_time;
	safestrncpy(st->message, message, MESSAGE_SIZE);
	safestrncpy(st->name, sd->status.name, NAME_LENGTH);

	st->id = st->vender_id;
	st->type = BL_STALL;
	st->m = sd->m;
	st->x = xPos;
	st->y = yPos;

	st->vd.look[LOOK_BASE] = sd->vd.look[LOOK_BASE];
	st->vd.look[LOOK_WEAPON] = sd->vd.look[LOOK_WEAPON];
	st->vd.look[LOOK_SHIELD] = sd->vd.look[LOOK_SHIELD];
	st->vd.look[LOOK_HEAD_TOP] = sd->vd.look[LOOK_HEAD_TOP];
	st->vd.look[LOOK_HEAD_MID] = sd->vd.look[LOOK_HEAD_MID];
	st->vd.look[LOOK_HEAD_BOTTOM] = sd->vd.look[LOOK_HEAD_BOTTOM];
	st->vd.look[LOOK_HAIR] = sd->vd.look[LOOK_HAIR];
	st->vd.look[LOOK_HAIR_COLOR] = sd->vd.look[LOOK_HAIR_COLOR];
	st->vd.look[LOOK_CLOTHES_COLOR] = sd->vd.look[LOOK_CLOTHES_COLOR];
	st->vd.look[LOOK_BODY2] = sd->vd.look[LOOK_BODY2];
	st->vd.sex = sd->vd.sex;
	st->vd.look[LOOK_ROBE] = sd->vd.look[LOOK_ROBE];

	Sql_EscapeString( mmysql_handle, message_sql, st->message );

	if( Sql_Query( mmysql_handle, "INSERT INTO `%s`(`id`, `uid`, `char_id`, `type`, `class`, `sex`, `map`, `x`, `y`,"
		                          "`title`, `hair`, `hair_color`, `body`, `weapon`, `shield`, `head_top`, `head_mid`, `head_bottom`, `robe`,"
								  "`clothes_color`, `name`, `expire_time`) "
		"VALUES( %d, %d, %d, %d, %d, '%c', '%s', %d, %d, '%s', %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, '%s', %u  );",
		stalls_table, st->vender_id, st->unique_id, st->owner_id, st->type, st->vd.look[LOOK_BASE], st->vd.sex == SEX_FEMALE ? 'F' : 'M', mapindex_id2name(st->m), st->x, st->y,
		message_sql, st->vd.look[LOOK_HAIR], st->vd.look[LOOK_HAIR_COLOR], st->vd.look[LOOK_BODY2], st->vd.look[LOOK_WEAPON], st->vd.look[LOOK_SHIELD], st->vd.look[LOOK_HEAD_TOP], st->vd.look[LOOK_HEAD_MID], st->vd.look[LOOK_HEAD_BOTTOM], st->vd.look[LOOK_ROBE],
		st->vd.look[LOOK_CLOTHES_COLOR], st->name, st->expire_time) != SQL_SUCCESS ) {
		Sql_ShowDebug(mmysql_handle);
	}

	StringBuf_Init(&buf);
	StringBuf_Printf(&buf, "INSERT INTO `%s`(`stalls_id`,`nameid`,`amount`,`price`) VALUES",stalls_buying_items_table);
	for (j = 0; j < i; j++) {
		StringBuf_Printf(&buf, "(%d, %u, %d, %d)",
			st->vender_id, st->itemId[j], st->amount[j], st->price[j]);
		if (j < i-1)
			StringBuf_AppendStr(&buf, ",");
	}
	if (SQL_ERROR == Sql_QueryStr(mmysql_handle, StringBuf_Value(&buf)))
		Sql_ShowDebug(mmysql_handle);
	StringBuf_Destroy(&buf);

	st->timer = add_timer(gettick() + (st->expire_time - time(nullptr)) * 1000,
				stall_timeout, st->id, 0);

	clif_stall_showunit(sd,st);
	clif_buyingstall_entry(sd,st->vender_id,st->message);
	clif_stall_ui_close(sd,101,STALLSTORE_OK);

	if(map_addblock(st))
		return -1;
	status_change_init(st);
	map_addiddb(st);
	stall_db.push_back(st);

	return 0;
}
#endif

bool stall_isStallOpen(uint32 CID, short type){

	auto itStalls = std::find_if(stall_db.begin(), stall_db.end(), [&](s_stall_data *const & itst) {
						return (CID == itst->owner_id && type == itst->type);
					});

	if(itStalls != 	stall_db.end()){
		return true;
	}

	return false;
}

/**
* Player request a stall's item list (a vending stall)
* @param sd : player requestion the list
* @param id : vender account id (gid)
*/
void stall_vending_listreq(map_session_data* sd, int32 id)
{
	nullpo_retv(sd);
	struct s_stall_data* st;

	if( (st = map_id2st(id)) == NULL )
		return;

	if (!pc_can_give_items(sd)) { //check if both GMs are allowed to trade
		clif_displaymessage(sd->fd, msg_txt(sd,246));
		return;
	}

	clif_stall_vending_list( sd, st );
}

/**
* Player request a stall's item list (a buying stall)
* @param sd : player requestion the list
* @param id : vender account id (gid)
*/
void stall_buying_listreq(map_session_data* sd, int32 id)
{
	nullpo_retv(sd);
	struct s_stall_data* st;

	if( !battle_config.feature_buying_store || pc_istrading(sd) )
	{// not allowed to sell
		return;
	}

	if( !pc_can_give_items(sd) )
	{// custom: GM is not allowed to sell
		clif_displaymessage(sd->fd, msg_txt(sd,246));
		return;
	}

	if( (st = map_id2st(id)) == NULL )
		return;

	clif_stall_buying_list( sd, st );
}

/**
* Purchase item(s) from a stall
* @param sd : buyer player session
* @param aid : char id of vender
* @param uid : stall unique id
* @param data : items data who would like to purchase \n
*	data := {<index>.w <amount>.w }[count]
* @param count : number of different items he's trying to buy
*/
void stall_vending_purchasereq(map_session_data* sd, int32 aid, int32 uid, const uint8* data, int32 count)
{
	int32 i, w;
	double z;
	struct s_stall_data* st = map_id2st(uid);

	nullpo_retv(sd);
	if( st == NULL )
		return; // invalid shop

	if(!stall_isStallOpen(st->owner_id, st->type)){
		clif_displaymessage(sd->fd, "This stall is not opened anymore.");
		return;
	}

	if( st->vender_id != uid || st->unique_id != aid ) { // shop has changed
		clif_buyvending( *sd, 0, 0, PURCHASEMC_STORE_INCORRECT );  // store information was incorrect
		return;
	}

	if( !searchstore_queryremote(*sd, st->unique_id) && (sd->m != st->m || !check_distance_bl(sd, st, AREA_SIZE)) )
		return; // shop too far away

	if( count < 1 || count > MAX_STALL_SLOT || count > st->vend_num )
		return; // invalid amount of purchased items

	// some checks
	z = 0.; // zeny counter
	w = 0;  // weight counter
	for( i = 0; i < count; i++ ) {
		short amount = *(uint16*)(data + 4*i + 0);
		short idx    = *(uint16*)(data + 4*i + 2);
		idx -= 1;

		if( amount <= 0 )
			return;

		// check of item index
		if( idx < 0 || idx >= st->vend_num )
			return;

		// items has been bought by anyone else
		if( st->items_inventory[idx].amount <= 0 )
			return;

		z += ((double)st->price[idx] * (double)amount);
		if( z > (double)sd->status.zeny || z < 0. || z > (double)MAX_ZENY ) {
			clif_buyvending( *sd, idx, amount, PURCHASEMC_NO_ZENY); // you don't have enough zeny
			return;
		}

		w += itemdb_weight(st->items_inventory[idx].nameid) * amount;
		if( w + sd->weight > sd->max_weight ) {
			clif_buyvending( *sd, idx, amount, PURCHASEMC_OVERWEIGHT); // you can not buy, because overweight
			return;
		}

		//Check to see if cart/vend info is in sync.
		if( amount > st->items_inventory[idx].amount ){
			clif_buyvending( *sd, idx, st->items_inventory[idx].amount, PURCHASEMC_OUT_OF_STOCK); // not enough quantity
			return;
		}
	}

	pc_payzeny(sd, (int32)z, LOG_TYPE_VENDING);
	achievement_update_objective(sd, AG_SPEND_ZENY, 1, (int32)z);

	struct mail_message msg_buyer = {};
	msg_buyer.dest_id = sd->status.char_id;
	safestrncpy( msg_buyer.send_name, "<MSG>2943</MSG>", NAME_LENGTH );
	safestrncpy( msg_buyer.title, "<MSG>2938</MSG>", MAIL_TITLE_LENGTH );

	msg_buyer.status = MAIL_NEW;
	msg_buyer.type = MAIL_INBOX_NORMAL;
	msg_buyer.timestamp = time( nullptr );

	struct mail_message msg_vendor = {};
	msg_vendor.dest_id = st->owner_id;
	msg_vendor.zeny = 0;
	safestrncpy( msg_vendor.send_name, "<MSG>2943</MSG>", NAME_LENGTH );
	safestrncpy( msg_vendor.title, "<MSG>2937</MSG>", MAIL_TITLE_LENGTH );

	msg_vendor.status = MAIL_NEW;
	msg_vendor.type = MAIL_INBOX_NORMAL;
	msg_vendor.timestamp = time( nullptr );

	char timestring[23];
	time_t curtime;
	time(&curtime);
	strftime(timestring, 22, "%m/%d/%Y, %H:%M", localtime(&curtime));

	std::ostringstream stream;
	stream << "<MSG>2932</MSG>" << timestring << "\r\n\r\n";

	uint32 totalfee = 0;
	uint32 totalprice = 0;
	for( i = 0; i < count; i++ ) {
		short amount = *(uint16*)(data + 4*i + 0);
		short idx    = *(uint16*)(data + 4*i + 2);
		idx -= 1;
		z = 0.; // zeny counter

		st->items_inventory[idx].amount -= amount;

		memcpy(&msg_buyer.item[i],&st->items_inventory[idx],sizeof(struct item));
		msg_buyer.item[i].amount = amount;

		std::shared_ptr<item_data> item_data = item_db.find(st->items_inventory[idx].nameid);
		std::string itemlstr = item_db.create_item_link(item_data, true).c_str();
		stream << "<MSG>2933</MSG>" << itemlstr << "\r\n";

		stream << "<MSG>2935</MSG>" << st->price[idx] << "z \r\n";
		stream << "<MSG>2934</MSG>" << amount << "\r\n";

		uint32 price = st->price[idx] * amount;
		uint32 fee = price * STALL_TAX / 100;
		uint32 countedprice = st->price[idx] * amount;
		if(st->price[idx] > 10000000)
			countedprice = price - fee;

		stream << "<MSG>3185</MSG>" << fee << "\r\n";

		msg_vendor.zeny += countedprice;
		totalfee += fee;
		totalprice += price;
	}
	stream << "<MSG>3186</MSG>" << totalfee << "z \r\n";
	stream << "<MSG>2936</MSG>" << msg_vendor.zeny << "z \r\n";
	stream << "\0";

	std::stringstream buyer_stream(stream.str());
	std::string buyer_str;
	std::string line;
	while (std::getline(buyer_stream, line)) {
		if (line.find("<MSG>3185</MSG>") == std::string::npos &&
			line.find("<MSG>3186</MSG>") == std::string::npos &&
			line.find("<MSG>2936</MSG>") == std::string::npos) {
			buyer_str += line + "\r\n";
		}
	}
	std::stringstream buyer_str_add;
	buyer_str_add << "<MSG>2936</MSG>" << totalprice << "z \r\n";
	buyer_str += buyer_str_add.str();

	safestrncpy( msg_buyer.body, buyer_str.c_str(), MAIL_BODY_LENGTH );
	if(!intif_Mail_send( 0, &msg_buyer )){
		stall_mail_db.push_back(msg_buyer);
	}

	safestrncpy( msg_vendor.body, const_cast<char*>(stream.str().c_str()), MAIL_BODY_LENGTH );
	if(!intif_Mail_send( 0, &msg_vendor )){
		stall_mail_db.push_back(msg_vendor);
	}

	bool remain_items = false;
	for( i = 0; i < st->vend_num; i++ ){
		if(st->items_inventory[i].amount > 0){
			remain_items = true;
			break;
		}
	}

	//Always save BOTH: customer (buyer) and vender
	if( save_settings&CHARSAVE_VENDING ) {
		chrif_save(sd, CSAVE_INVENTORY|CSAVE_CART);
		if(!remain_items){
			stall_remove(st);
		} else
			stall_vending_save(st);
	}
}

/**
* Sell item(s) to a buying a stall
* @param sd : buyer player session
* @param aid : char id of vender
* @param uid : stall unique id
* @param data : items data who would like to purchase \n
*	data := {<index>.w <amount>.w }[count]
* @param count : number of different items he's trying to buy
*/
void stall_buying_purchasereq(map_session_data* sd, int32 aid, int32 uid, const struct PACKET_CZ_REQ_TRADE_BUYING_STORE_sub* itemlist, uint32 count )
{
	int32 zeny = 0;
	struct s_stall_data* st = map_id2st(uid);

	nullpo_retv(sd);
	if( st == NULL )
		return; // invalid shop

	if(!stall_isStallOpen(st->owner_id, st->type)){
		clif_displaymessage(sd->fd, "This stall is not opened anymore.");
		return;
	}

	if( st->vender_id != uid || st->unique_id != aid ) { // shop has changed
		clif_buyingstore_trade_failed_seller(sd, BUYINGSTORE_TRADE_SELLER_FAILED, 0);
		return;
	}

	if( !battle_config.feature_buying_store || pc_istrading(sd) )
	{// not allowed to sell
		clif_buyingstore_trade_failed_seller(sd, BUYINGSTORE_TRADE_SELLER_FAILED, 0);
		return;
	}

	if( !pc_can_give_items(sd) )
	{// custom: GM is not allowed to sell
		clif_displaymessage(sd->fd, msg_txt(sd,246));
		clif_buyingstore_trade_failed_seller(sd, BUYINGSTORE_TRADE_SELLER_FAILED, 0);
		return;
	}

	if( !searchstore_queryremote(*sd, st->unique_id) && (sd->m != st->m || !check_distance_bl(sd, st, AREA_SIZE)) ){
		clif_buyingstore_trade_failed_seller(sd, BUYINGSTORE_TRADE_SELLER_FAILED, 0);
		return; // shop too far away
	}

	if( count < 1 || count > MAX_STALL_SLOT || count > st->vend_num )
		return; // invalid amount of purchased items

	// some checks
	for( int32 i = 0; i < count; i++ ){
		const struct PACKET_CZ_REQ_TRADE_BUYING_STORE_sub* item = &itemlist[i];

		// duplicate check. as the client does this too, only malicious intent should be caught here
		for( int32 k = 0; k < i; k++ ){
			// duplicate
			if( itemlist[k].index == item->index && k != i ){
				ShowWarning( "stall_buying_purchasereq: Found duplicate item on selling list (prevnameid=%u, prevamount=%hu, nameid=%u, amount=%hu, account_id=%d, char_id=%d).\n", itemlist[k].itemId, itemlist[k].amount, item->itemId, item->amount, sd->status.account_id, sd->status.char_id );
				clif_buyingstore_trade_failed_seller( sd, BUYINGSTORE_TRADE_SELLER_FAILED, item->itemId );
				return;
			}
		}

		int32 index = item->index - 2; // TODO: clif::server_index

		if( item->amount <= 0 )
			return;

		// invalid input
		if( index < 0 || index >= ARRAYLENGTH( sd->inventory.u.items_inventory ) || sd->inventory_data[index] == NULL || sd->inventory.u.items_inventory[index].nameid != item->itemId || sd->inventory.u.items_inventory[index].amount < item->amount ){
			clif_buyingstore_trade_failed_seller( sd, BUYINGSTORE_TRADE_SELLER_FAILED, item->itemId );
			return;
		}

		// non-tradable item
		if( sd->inventory.u.items_inventory[index].expire_time || ( sd->inventory.u.items_inventory[index].bound && !pc_can_give_bounded_items( sd ) ) || memcmp( sd->inventory.u.items_inventory[index].card, buyingstore_blankslots, sizeof( buyingstore_blankslots ) ) ){
			clif_buyingstore_trade_failed_seller( sd, BUYINGSTORE_TRADE_SELLER_FAILED, item->itemId );
			return;
		}

		int32 listidx;
		for(int32 k = 0 ; k < st->vend_num; k++){
			if(item->itemId == st->itemId[k]){
				listidx = k;
				break;
			}
		}

		// items has been sell by anyone else
		if( st->amount[listidx] <= 0 || st->amount[listidx] < item->amount){
			clif_buyingstore_trade_failed_seller( sd, BUYINGSTORE_TRADE_SELLER_COUNT, item->itemId );
			return;
		}

		zeny += item->amount * st->price[listidx];
	}

	struct mail_message msg_vendor = {};
	msg_vendor.dest_id = sd->status.char_id;
	msg_vendor.zeny = 0;
	safestrncpy( msg_vendor.send_name, "<MSG>2943</MSG>", NAME_LENGTH );
	safestrncpy( msg_vendor.title, "<MSG>2937</MSG>", MAIL_TITLE_LENGTH );

	msg_vendor.status = MAIL_NEW;
	msg_vendor.type = MAIL_INBOX_NORMAL;
	msg_vendor.timestamp = time( nullptr );

	struct mail_message msg_buyer = {};
	msg_buyer.dest_id = st->owner_id;
	safestrncpy( msg_buyer.send_name, "<MSG>2943</MSG>", NAME_LENGTH );
	safestrncpy( msg_buyer.title, "<MSG>2938</MSG>", MAIL_TITLE_LENGTH );

	msg_buyer.status = MAIL_NEW;
	msg_buyer.type = MAIL_INBOX_NORMAL;
	msg_buyer.timestamp = time( nullptr );

	char timestring[23];
	time_t curtime;
	time(&curtime);
	strftime(timestring, 22, "%m/%d/%Y, %H:%M", localtime(&curtime));

	std::ostringstream stream;
	stream << "<MSG>2932</MSG>" << timestring << "\r\n\r\n";

	// process item list
	for( int32 i = 0; i < count; i++ ){
		const struct PACKET_CZ_REQ_TRADE_BUYING_STORE_sub* item = &itemlist[i];

		int32 listidx;
		for(int32 k = 0 ; k < st->vend_num; k++){
			if(item->itemId == st->itemId[k]){
				listidx = k;
				break;
			}
		}

		int32 index = item->index - 2; // TODO: clif::server_index

		// move item
		memcpy(&msg_buyer.item[i],&sd->inventory.u.items_inventory[index],sizeof(struct item));
		msg_buyer.item[i].amount = item->amount;

		pc_delitem(sd, index, item->amount, 1, 0, LOG_TYPE_BUYING_STORE);
		st->amount[listidx] -= item->amount;

		std::shared_ptr<item_data> item_data = item_db.find(item->itemId);
		std::string itemlstr = item_db.create_item_link(item_data, true).c_str();
		stream << "<MSG>2933</MSG>" << itemlstr << "\r\n";

		stream << "<MSG>2935</MSG>" << st->price[listidx] << "z \r\n";
		stream << "<MSG>2934</MSG>" << item->amount << "\r\n";

		msg_vendor.zeny += item->amount * st->price[listidx];
		clif_buyingstore_delete_item(sd, index, item->amount, st->price[listidx]);
	}
	stream << "<MSG>2936</MSG>" << msg_vendor.zeny << "z \r\n";
	stream << "\0";

	safestrncpy( msg_vendor.body, const_cast<char*>(stream.str().c_str()), MAIL_BODY_LENGTH );
	if(!intif_Mail_send( 0, &msg_vendor )){
		stall_mail_db.push_back(msg_vendor);
	}

	safestrncpy( msg_buyer.body, const_cast<char*>(stream.str().c_str()), MAIL_BODY_LENGTH );
	if(!intif_Mail_send( 0, &msg_buyer )){
		stall_mail_db.push_back(msg_buyer);
	}
	
	bool remain_items = false;
	for( int32 i = 0; i < st->vend_num; i++ ){
		if(st->amount[i] > 0){
			remain_items = true;
			break;
		}
	}

	//Always save BOTH: customer (buyer) and vender
	if( save_settings&CHARSAVE_VENDING ) {
		chrif_save(sd, CSAVE_INVENTORY|CSAVE_CART);
		if(!remain_items){
			stall_remove(st);
		} else
			stall_buying_save(st);
	}
}

void stall_close(map_session_data* sd, int32 uid){
	auto itStalls = std::find_if(stall_db.begin(), stall_db.end(), [&](s_stall_data *const & itst) {
						return (sd->status.char_id == itst->owner_id && uid == itst->unique_id);
					});

	if(itStalls != stall_db.end()){
		switch((*itStalls)->type){
			case 0:
				stall_vending_getbackitems(*itStalls);
				clif_stall_ui_close(sd,100,STALLSTORE_OK);
				break;
			case 1:
				stall_buying_getbackzeny(*itStalls);
				clif_stall_ui_close(sd,101,STALLSTORE_OK);
				break;
		}
		stall_remove(*itStalls);
	}
}

void stall_vending_save(struct s_stall_data* st){
	for(int32 i = 0; i < st->vend_num; i++){
		if( Sql_Query( mmysql_handle, "UPDATE `%s` SET `amount` = %d WHERE `stalls_id` = %d AND `index` = %d;",
			stalls_vending_items_table, st->items_inventory[i].amount, st->vender_id, i) != SQL_SUCCESS ) {
			Sql_ShowDebug(mmysql_handle);
		}
	}
}

void stall_buying_save(struct s_stall_data* st){
	for(int32 i = 0; i < st->vend_num; i++){
		if( Sql_Query( mmysql_handle, "UPDATE `%s` SET `amount` = %d WHERE `stalls_id` = %d AND `nameid` = %d;",
			stalls_buying_items_table, st->amount[i], st->vender_id, st->itemId[i]) != SQL_SUCCESS ) {
			Sql_ShowDebug(mmysql_handle);
		}
	}
}

void stall_vending_getbackitems(struct s_stall_data* st){
	struct mail_message msg_vendor = {};

	msg_vendor.dest_id = st->owner_id;
	safestrncpy( msg_vendor.send_name, "<MSG>2943</MSG>", NAME_LENGTH );
	safestrncpy( msg_vendor.title, "<MSG>2939</MSG>", MAIL_TITLE_LENGTH );

	msg_vendor.status = MAIL_NEW;
	msg_vendor.type = MAIL_INBOX_NORMAL;
	msg_vendor.timestamp = time( nullptr );

	char timestring[23];
	time_t curtime;
	time(&curtime);
	strftime(timestring, 22, "%m/%d/%Y, %H:%M", localtime(&curtime));

	std::ostringstream stream;
	stream << "<MSG>2946</MSG> " << timestring << "\r\n";
	stream << "\r\n<MSG>2942</MSG>\r\n\r\n";

	int32 mail_index = 0;
	for( int32 i = 0; i < st->vend_num; i++ ) {
		if(st->items_inventory[i].amount > 0){
			memcpy(&msg_vendor.item[mail_index],&st->items_inventory[i],sizeof(struct item));
			msg_vendor.item[mail_index].amount = st->items_inventory[i].amount;

			std::shared_ptr<item_data> item_data = item_db.find(st->items_inventory[i].nameid);
			std::string itemlstr = item_db.create_item_link(item_data, true).c_str();
			stream << "<MSG>2944</MSG> " << itemlstr << "\r\n";
			stream << "<MSG>2945</MSG> " << st->items_inventory[i].amount << "\r\n";
			mail_index++;
		}
	}
	stream << "\0";

	safestrncpy( msg_vendor.body, const_cast<char*>(stream.str().c_str()), MAIL_BODY_LENGTH );
	if(!intif_Mail_send( 0, &msg_vendor )){
		stall_mail_db.push_back(msg_vendor);
	}
}

void stall_buying_getbackzeny(struct s_stall_data* st){
	struct mail_message msg_buyer = {};

	msg_buyer.dest_id = st->owner_id;
	safestrncpy( msg_buyer.send_name, "<MSG>2943</MSG>", NAME_LENGTH );
	safestrncpy( msg_buyer.title, "<MSG>2940</MSG>", MAIL_TITLE_LENGTH );

	msg_buyer.status = MAIL_NEW;
	msg_buyer.type = MAIL_INBOX_NORMAL;
	msg_buyer.timestamp = time( nullptr );

	char timestring[23];
	time_t curtime;
	time(&curtime);
	strftime(timestring, 22, "%m/%d/%Y, %H:%M", localtime(&curtime));

	std::ostringstream stream;
	stream << "<MSG>2946</MSG> " << timestring << "\r\n";

	for( int32 i = 0; i < st->vend_num; i++ ) {
		uint32 price = st->amount[i] * st->price[i];
		if(st->price[i] > 10000000) // not tax if not bought
			price = price * (100 + STALL_TAX) / 100;
		msg_buyer.zeny += price;
	}

	stream << "\r\n<MSG>2941</MSG>" << msg_buyer.zeny << " z\r\n";
	stream << "\0";

	safestrncpy( msg_buyer.body, const_cast<char*>(stream.str().c_str()), MAIL_BODY_LENGTH );
	if(!intif_Mail_send( 0, &msg_buyer )){
		stall_mail_db.push_back(msg_buyer);
	}
}

void stall_remove(struct s_stall_data* st){
	if( Sql_Query( mmysql_handle, "DELETE FROM `%s` WHERE `id` = %d;", stalls_table, st->vender_id) != SQL_SUCCESS ) {
			Sql_ShowDebug(mmysql_handle);
	}

	map_session_data *vsd, *bsd;

	switch(st->type){
		case 0: {
			if (Sql_Query(mmysql_handle, "DELETE FROM `%s` WHERE `stalls_id` = %d;", stalls_vending_items_table, st->vender_id) != SQL_SUCCESS) {
				Sql_ShowDebug(mmysql_handle);
			}
			vsd = map_id2sd(st->vid);
			if (vsd)
				clif_msg(*vsd, MSI_CANNOT_ACCESS_BY_WEIGHTOVER_80);
			st->vid = 0;
		}
			break;
		case 1: {
			if( Sql_Query( mmysql_handle, "DELETE FROM `%s` WHERE `stalls_id` = %d;", stalls_buying_items_table, st->vender_id ) != SQL_SUCCESS ) {
				Sql_ShowDebug(mmysql_handle);
			}
			bsd = map_id2sd(st->bid);
			if (bsd)
				clif_msg(*bsd, MSI_CANNOT_ACCESS_BY_WEIGHTOVER_80);
			st->bid = 0;
		}
			break;
	}
	if(st->timer != INVALID_TIMER)
		delete_timer(st->timer, stall_timeout);

	stall_db.erase(
	std::remove_if(stall_db.begin(), stall_db.end(), [&](s_stall_data * const & itst) {
		return st->vender_id == itst->vender_id;
	}),
	stall_db.end());

	clif_clearunit_area(*st,CLR_OUTSIGHT);
	map_delblock(st);
	map_freeblock(st);
}

TIMER_FUNC (stall_timeout){
	struct s_stall_data* st = map_id2st(id);
	nullpo_ret(st);

	// Remove the current timer
	st->timer = INVALID_TIMER;

	auto itStalls = std::find_if(stall_db.begin(), stall_db.end(), [&](s_stall_data *const & itst) {
						return st->unique_id == itst->unique_id;
					});

	if(itStalls != 	stall_db.end()){
		switch((*itStalls)->type){
			case 0:
				stall_vending_getbackitems(*itStalls);
				break;
			case 1:
				stall_buying_getbackzeny(*itStalls);
				break;
		}
		stall_remove(*itStalls);
	}

	return 0;
}

/**
* Searches for all items in a stall, that match given ids, price and possible cards.
* @param sd : The vender session to search into
* @param s : parameter of the search (see s_search_store_search)
* @return Whether or not the search should be continued.
*/
bool stall_searchall(map_session_data* sd, const struct s_search_store_search* s, const struct s_stall_data* st, short type)
{
	int32 c, slot;
	uint32 cidx;

	if( !st->type == type ) // not good type
		return true;

	if(st->type == 0){
		for( int32 j = 0; j < s->item_count; j++ ) {
			for( int32 i = 0; i < st->vend_num; i++ ) {
				if(st->items_inventory[i].amount > 0
					&& s->itemlist[j].itemId == st->items_inventory[i].nameid){

					if( s->min_price && s->min_price > st->price[i] ) { // too low price
						continue;
					}

					if( s->max_price && s->max_price < st->price[i] ) { // too high price
						continue;
					}

					if( s->card_count ) { // check cards
						if( itemdb_isspecial(st->items_inventory[i].card[0]) ) { // something, that is not a carded
							continue;
						}
						slot = itemdb_slots(st->items_inventory[i].nameid);

						for( c = 0; c < slot && st->items_inventory[i].card[c]; c ++ ) {
							ARR_FIND( 0, s->card_count, cidx, s->cardlist[cidx].itemId == st->items_inventory[i].card[c] );
							if( cidx != s->card_count ) { // found
								break;
							}
						}

						if( c == slot || !st->items_inventory[i].card[c] ) { // no card match
							continue;
						}
					}

					// Check if the result set is full
					if( s->search_sd->searchstore.items.size() >= (uint32)battle_config.searchstore_maxresults ){
						return false;
					}

					std::shared_ptr<s_search_store_info_item> ssitem = std::make_shared<s_search_store_info_item>();

					ssitem->store_id = st->vender_id;
					ssitem->account_id = st->unique_id;
					safestrncpy( ssitem->store_name, st->message, sizeof( ssitem->store_name ) );
					ssitem->nameid = st->items_inventory[i].nameid;
					ssitem->amount = st->items_inventory[i].amount;
					ssitem->price = st->price[i];
					for( int32 j = 0; j < MAX_SLOTS; j++ ){
						ssitem->card[j] = st->items_inventory[i].card[j];
					}
					ssitem->refine = st->items_inventory[i].refine;
					ssitem->enchantgrade = st->items_inventory[i].enchantgrade;

					s->search_sd->searchstore.items.push_back( ssitem );
				}
			}
		}
	}
	if(st->type == 1){
		for( int32 j = 0; j < s->item_count; j++ ) {
			for( int32 i = 0; i < st->vend_num; i++ ) {
				if(st->amount[i] > 0
					&& s->itemlist[j].itemId == st->itemId[i]){
					if( s->min_price && s->min_price > st->price[i] ) { // too low price
						continue;
					}

					if( s->max_price && s->max_price < st->price[i] ) { // too high price
						continue;
					}

					if( s->card_count ) { // check cards
						;// ignore cards, as there cannot be any
					}

					// Check if the result set is full
					if( s->search_sd->searchstore.items.size() >= (uint32)battle_config.searchstore_maxresults ){
						return false;
					}

					std::shared_ptr<s_search_store_info_item> ssitem = std::make_shared<s_search_store_info_item>();

					ssitem->store_id = st->vender_id;
					ssitem->account_id = st->unique_id;
					safestrncpy( ssitem->store_name, st->message, sizeof( ssitem->store_name ) );
					ssitem->nameid = st->itemId[i];
					ssitem->amount = st->amount[i];
					ssitem->price = st->price[i];
					for( int32 j = 0; j < MAX_SLOTS; j++ ){
						ssitem->card[j] = 0;
					}
					ssitem->refine = 0;
					ssitem->enchantgrade = 0;

					s->search_sd->searchstore.items.push_back( ssitem );
				}
			}
		}
	}

	return true;
}

TIMER_FUNC(stall_mail_queue){
	if(stall_mail_db.size() > 0){
		for (auto it = stall_mail_db.begin(); it != stall_mail_db.end(); it++){
			// remove odd numbers
			if (intif_Mail_send( 0, &(*it) )){
				// Notice that the iterator is decremented after it is passed
				// to `erase()` but before `erase()` is executed
				stall_mail_db.erase(it--);
			}
		}
	}

	add_timer(gettick() + 60000, stall_mail_queue, 0, 0); // check queue every minute

	return 0;
}

TIMER_FUNC(stall_init){
	DBIterator *iter = NULL;
	struct s_stall_data *st = NULL;
	int32 i;
	std::vector<int32> stall_remove_list;

	if (Sql_Query(mmysql_handle,
		"SELECT `id`, `uid`, `char_id`, `type`, `class`, `sex`, `map`, `x`, `y`,"
		"`title`, `hair`, `hair_color`, `body`, `weapon`, `shield`, `head_top`, `head_mid`, `head_bottom`, `robe`,"
		"`clothes_color`, `name`, `expire_time` "
		"FROM `%s` ",
		stalls_table ) != SQL_SUCCESS )
	{
		Sql_ShowDebug(mmysql_handle);
		return 1;
	}

	// Init each stalls data
	while (SQL_SUCCESS == Sql_NextRow(mmysql_handle)) {
		size_t len;
		char* data;
		st = NULL;
		st = (struct s_stall_data*)aCalloc(1, sizeof(struct s_stall_data));
		Sql_GetData(mmysql_handle, 0, &data, NULL); st->vender_id = atoi(data);
		Sql_GetData(mmysql_handle, 1, &data, NULL); st->unique_id = atoi(data);
		Sql_GetData(mmysql_handle, 2, &data, NULL); st->owner_id = atoi(data);
		st->id = st->vender_id;
		Sql_GetData(mmysql_handle, 3, &data, NULL); st->type = atoi(data);
		Sql_GetData(mmysql_handle, 4, &data, NULL); st->vd.look[LOOK_BASE] = atoi(data);
		Sql_GetData(mmysql_handle, 5, &data, NULL); st->vd.sex = (data[0] == 'F') ? SEX_FEMALE : SEX_MALE;
		char esc_mapname[NAME_LENGTH*2+1];
		Sql_GetData(mmysql_handle, 6, &data, &len); safestrncpy(esc_mapname, data, zmin(len + 1, MESSAGE_SIZE));
		st->m = mapindex_name2id(esc_mapname);
		Sql_GetData(mmysql_handle, 7, &data, NULL); st->x = atoi(data);
		Sql_GetData(mmysql_handle, 8, &data, NULL); st->y = atoi(data);
		Sql_GetData(mmysql_handle, 9, &data, &len); safestrncpy(st->message, data, zmin(len + 1, MESSAGE_SIZE));
		Sql_GetData(mmysql_handle, 10, &data, NULL); st->vd.look[LOOK_HAIR] = atoi(data);
		Sql_GetData(mmysql_handle, 11, &data, NULL); st->vd.look[LOOK_HAIR_COLOR] = atoi(data);
		Sql_GetData(mmysql_handle, 12, &data, NULL); st->vd.look[LOOK_BODY2] = atoi(data);
		Sql_GetData(mmysql_handle, 13, &data, NULL); st->vd.look[LOOK_WEAPON] = atoi(data);
		Sql_GetData(mmysql_handle, 14, &data, NULL); st->vd.look[LOOK_SHIELD] = atoi(data);
		Sql_GetData(mmysql_handle, 15, &data, NULL); st->vd.look[LOOK_HEAD_TOP] = atoi(data);
		Sql_GetData(mmysql_handle, 16, &data, NULL); st->vd.look[LOOK_HEAD_MID] = atoi(data);
		Sql_GetData(mmysql_handle, 17, &data, NULL); st->vd.look[LOOK_HEAD_BOTTOM] = atoi(data);
		Sql_GetData(mmysql_handle, 18, &data, NULL); st->vd.look[LOOK_ROBE] = atoi(data);
		Sql_GetData(mmysql_handle, 19, &data, NULL); st->vd.look[LOOK_CLOTHES_COLOR] = atoi(data);
		Sql_GetData(mmysql_handle, 20, &data, &len); safestrncpy(st->name, data, zmin(len + 1, MESSAGE_SIZE));
		Sql_GetData(mmysql_handle, 21, &data, NULL); st->expire_time = strtoul(data, nullptr, 10);
		st->type = BL_STALL;
		stall_db.push_back(st);
	}

	for (auto& itStalls : stall_db){
		int32 item_count = 0;

		switch(itStalls->type){
			case 0:{
				if (Sql_Query(mmysql_handle,
					"SELECT `nameid`,`amount`,`identify`,`refine`,`attribute`"
					",`card0`,`card1`,`card2`,`card3`"
					",`option_id0`,`option_val0`,`option_parm0`,`option_id1`,`option_val1`,`option_parm1`,`option_id2`,`option_val2`,`option_parm2`,`option_id3`,`option_val3`,`option_parm3`,`option_id4`,`option_val4`,`option_parm4`"
					",`expire_time`,`bound`,`unique_id`,`enchantgrade`,`price` "
					"FROM `%s` WHERE stalls_id = %d ",
					stalls_vending_items_table, itStalls->vender_id ) != SQL_SUCCESS )
				{
					Sql_ShowDebug(mmysql_handle);
					return 1;
				}

				while (SQL_SUCCESS == Sql_NextRow(mmysql_handle)) {
					char* data;
					struct item item;
					Sql_GetData(mmysql_handle, 0, &data, NULL); item.nameid = strtoul(data, nullptr, 10);
					Sql_GetData(mmysql_handle, 1, &data, NULL); item.amount = atoi(data);
					Sql_GetData(mmysql_handle, 2, &data, NULL); item.identify = atoi(data);
					Sql_GetData(mmysql_handle, 3, &data, NULL); item.refine = atoi(data);
					Sql_GetData(mmysql_handle, 4, &data, NULL); item.attribute = atoi(data);
					for( i = 0; i < MAX_SLOTS; ++i ){
						Sql_GetData(mmysql_handle, 5+i, &data, NULL);
						item.card[i] = atoi(data);
					}
					for( i = 0; i < MAX_ITEM_RDM_OPT; ++i ) {
						Sql_GetData(mmysql_handle, 5+MAX_SLOTS+i*3, &data, NULL); item.option[i].id = atoi(data);
						Sql_GetData(mmysql_handle, 5+MAX_SLOTS+i*3, &data, NULL); item.option[i].value = atoi(data);
						Sql_GetData(mmysql_handle, 5+MAX_SLOTS+i*3, &data, NULL); item.option[i].param = atoi(data);
					}
					Sql_GetData(mmysql_handle, 5+MAX_SLOTS+MAX_ITEM_RDM_OPT*3, &data, NULL); item.expire_time = strtoul(data, nullptr, 10);
					Sql_GetData(mmysql_handle, 6+MAX_SLOTS+MAX_ITEM_RDM_OPT*3, &data, NULL); item.bound = atoi(data);
					Sql_GetData(mmysql_handle, 7+MAX_SLOTS+MAX_ITEM_RDM_OPT*3, &data, NULL); item.unique_id = strtoull(data, nullptr, 10);;
					Sql_GetData(mmysql_handle, 8+MAX_SLOTS+MAX_ITEM_RDM_OPT*3, &data, NULL); item.enchantgrade = atoi(data);

					Sql_GetData(mmysql_handle, 9+MAX_SLOTS+MAX_ITEM_RDM_OPT*3, &data, NULL); itStalls->price[item_count] = atoi(data);
					memcpy(&itStalls->items_inventory[item_count],&item,sizeof(struct item));
					item_count++;
				}
			} break;
			case 1: {
				if (Sql_Query(mmysql_handle,
					"SELECT `nameid`,`amount`,`price` "
					"FROM `%s` WHERE stalls_id = %d ",
					stalls_buying_items_table, itStalls->vender_id ) != SQL_SUCCESS )
				{
					Sql_ShowDebug(mmysql_handle);
					return 1;
				}

				while (SQL_SUCCESS == Sql_NextRow(mmysql_handle)) {
					char* data;

					Sql_GetData(mmysql_handle, 0, &data, NULL); itStalls->itemId[item_count] = strtoul(data, nullptr, 10);
					Sql_GetData(mmysql_handle, 1, &data, NULL); itStalls->amount[item_count] = atoi(data);
					Sql_GetData(mmysql_handle, 2, &data, NULL); itStalls->price[item_count] = atoi(data);

					item_count++;
				}
			} break;
		}
		itStalls->vend_num = item_count;
		long int remain_time = static_cast<long int>(itStalls->expire_time - time(nullptr));

		if(item_count == 0 || remain_time < 0){

			if( Sql_Query( mmysql_handle, "DELETE FROM `%s` WHERE `id` = %d;", stalls_table, itStalls->vender_id ) != SQL_SUCCESS ) {
					Sql_ShowDebug(mmysql_handle);
			}

			stall_remove_list.push_back(itStalls->vender_id);

			if(remain_time < 0 && item_count > 0){
				switch(itStalls->type){
					case 0:
						stall_vending_getbackitems(itStalls);
						if( Sql_Query( mmysql_handle, "DELETE FROM `%s` WHERE `stalls_id` = %d;", stalls_vending_items_table, itStalls->vender_id ) != SQL_SUCCESS ) {
								Sql_ShowDebug(mmysql_handle);
						}
						break;
					case 1:
						stall_buying_getbackzeny(itStalls);
						if( Sql_Query( mmysql_handle, "DELETE FROM `%s` WHERE `stalls_id` = %d;", stalls_buying_items_table, itStalls->vender_id ) != SQL_SUCCESS ) {
								Sql_ShowDebug(mmysql_handle);
						}
						break;
				}
			}
			aFree(itStalls);
			continue;
		}

		itStalls->timer = add_timer(gettick() + (remain_time) * 1000,
			stall_timeout, itStalls->id, 0);

		if(map_addblock(itStalls))
			continue;
		status_change_init(itStalls);
		map_addiddb(itStalls);
	}

	if(stall_remove_list.size() > 0){
		for (auto& itStalls : stall_remove_list){
			stall_db.erase(
			std::remove_if(stall_db.begin(), stall_db.end(), [&](s_stall_data * const & itst) {
				return itStalls == itst->vender_id;
			}),
			stall_db.end());
		}
	}
	stall_remove_list.clear();

	ShowStatus("Done loading '" CL_WHITE "%zu" CL_RESET "' vending stalls.\n", stall_db.size());

	return 0;
}

void do_init_stall(void)
{
	add_timer(gettick() + 1, stall_init, 0, 0); // need to delay for send mails if timeout because it doesn't see the char server up...
	add_timer(gettick() + 60000, stall_mail_queue, 0, 0); // check mail queue every minute in case something goes wrong with char server
}

void do_final_stall(void)
{
	for (auto& i : stall_db)
		aFree(i);

	stall_db.clear();
	stall_mail_db.clear();
}
