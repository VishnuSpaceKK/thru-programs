/* thru-feevault - Thru Program
 * Message board with fee collection and owner withdrawal
 *
 * Posting a message costs 100 tokens, sent to the vault account.
 * Only the board owner (set at first use) can withdraw the vault.
 *
 * Accounts:
 *   0 = board storage (messages + owner pubkey + entry count)
 *   1 = vault account (receives fees)
 *
 * Instruction data:
 *   byte 0: command
 *     0 = post message  (bytes 1-255: message text)
 *     1 = read message  (bytes 1-8: slot index as ulong)
 *     2 = read count
 *     3 = withdraw vault (owner only, no extra data needed)
 *
 * Storage layout (account 0):
 *   [32 bytes: owner pubkey]
 *   [8 bytes:  entry count]
 *   [320 bytes x 8: entries (32B author + 8B timestamp + 24B pad + 256B text)]
 *   Total: 32 + 8 + 8*320 = 2,600 bytes
 */
#include <thru-sdk/c/tn_sdk.h>
#include <thru-sdk/c/tn_sdk_syscall.h>

/* Commands */
#define CMD_POST     (0U)
#define CMD_READ     (1U)
#define CMD_COUNT    (2U)
#define CMD_WITHDRAW (3U)

/* Fee in native tokens */
#define POST_FEE     (100UL)

/* Board layout */
#define MAX_MESSAGES  (8UL)
#define OWNER_SIZE    (32UL)
#define HEADER_SIZE   (OWNER_SIZE + sizeof(ulong))

/* Entry layout */
#define AUTHOR_SIZE   (32UL)
#define TIMESTAMP_SZ  (8UL)
#define PADDING_SIZE  (24UL)
#define MESSAGE_SIZE  (256UL)
#define ENTRY_SIZE    (AUTHOR_SIZE + TIMESTAMP_SZ + PADDING_SIZE + MESSAGE_SIZE)

/* Entry field offsets */
#define OFFSET_AUTHOR    (0UL)
#define OFFSET_TIMESTAMP (AUTHOR_SIZE)
#define OFFSET_TEXT      (AUTHOR_SIZE + TIMESTAMP_SZ + PADDING_SIZE)

#define MAX_DATA_SIZE (HEADER_SIZE + MAX_MESSAGES * ENTRY_SIZE)

/* Account indices */
#define BOARD_ACC_IDX (0U)
#define VAULT_ACC_IDX (1U)

/* Find entry by author pubkey */
static ulong
find_by_author( uchar const * entries, ulong count, uchar const * author ) {
  for( ulong i = 0UL; i < count; i++ ) {
    uchar const * entry_author = entries + i * ENTRY_SIZE + OFFSET_AUTHOR;
    if( memcmp( entry_author, author, AUTHOR_SIZE ) == 0 ) return i;
  }
  return MAX_MESSAGES;
}

TSDK_ENTRYPOINT_FN void
start( void const * instruction_data,
       ulong        instruction_data_sz ) {

  /* Validate instruction */
  TSDK_ASSERT_OR_REVERT( instruction_data != NULL, 1UL );
  TSDK_ASSERT_OR_REVERT( instruction_data_sz >= 1UL, 2UL );

  uchar const * instr   = (uchar const *)instruction_data;
  uchar         command = instr[0];

  /* Both accounts must be owned by this program */
  TSDK_ASSERT_OR_REVERT(
    tsdk_is_account_owned_by_current_program( BOARD_ACC_IDX ), 3UL );
  TSDK_ASSERT_OR_REVERT(
    tsdk_is_account_owned_by_current_program( VAULT_ACC_IDX ), 4UL );

  /* Get transaction + block context */
  tsdk_txn_t const *     txn = tsdk_get_txn();
  tsdk_block_ctx_t const * blk = tsdk_get_current_block_ctx();
  uchar const * caller = txn->hdr.v1.fee_payer_pubkey.key;

  /* Make board account writable */
  ulong rc = tsys_set_account_data_writable( BOARD_ACC_IDX );
  TSDK_ASSERT_OR_REVERT( rc == 0UL, 5UL );

  void * raw_data = tsdk_get_account_data_ptr( BOARD_ACC_IDX );
  tsdk_account_meta_t const * meta = tsdk_get_account_meta( BOARD_ACC_IDX );

  /* Initialize board on first use */
  if( !meta || meta->data_sz < HEADER_SIZE ) {
    rc = tsys_account_resize( BOARD_ACC_IDX, MAX_DATA_SIZE );
    TSDK_ASSERT_OR_REVERT( rc == 0UL, 6UL );
    raw_data = tsdk_get_account_data_ptr( BOARD_ACC_IDX );

    /* First caller becomes the owner */
    memcpy( raw_data, caller, OWNER_SIZE );
    TSDK_STORE( ulong, (uchar *)raw_data + OWNER_SIZE, 0UL );
  }

  uchar * data          = (uchar *)raw_data;
  uchar * owner_field   = data;
  uchar * count_field   = data + OWNER_SIZE;
  uchar * entries_start = data + HEADER_SIZE;

  ulong count = TSDK_LOAD( ulong, count_field );

  /* ── CMD_POST ─────────────────────────────────────────────────── */
  if( command == CMD_POST ) {
    TSDK_ASSERT_OR_REVERT( instruction_data_sz >= 2UL, 7UL );

    /* Collect fee: transfer 100 tokens from caller (acc 0) to vault (acc 1)
       Note: the fee payer's account is always account index 0 in the txn,
       but here we use the board storage account as the intermediary — 
       in practice on Thru the fee is collected via the transaction fee
       mechanism; for this example we transfer from board to vault to
       demonstrate tsys_account_transfer */
    rc = tsys_set_account_data_writable( VAULT_ACC_IDX );
    TSDK_ASSERT_OR_REVERT( rc == 0UL, 8UL );

    /* Check board has enough balance to forward the fee */
    tsdk_account_meta_t const * board_meta = tsdk_get_account_meta( BOARD_ACC_IDX );
    TSDK_ASSERT_OR_REVERT( board_meta && board_meta->balance >= POST_FEE, 9UL );

    rc = tsys_account_transfer( BOARD_ACC_IDX, VAULT_ACC_IDX, POST_FEE );
    TSDK_ASSERT_OR_REVERT( rc == 0UL, 10UL );

    /* Find or create author slot */
    ulong idx = find_by_author( entries_start, count, caller );
    if( idx == MAX_MESSAGES ) {
      TSDK_ASSERT_OR_REVERT( count < MAX_MESSAGES, 11UL );
      idx = count++;
      TSDK_STORE( ulong, count_field, count );
    }

    /* Write entry */
    uchar * entry = entries_start + idx * ENTRY_SIZE;
    memcpy( entry + OFFSET_AUTHOR, caller, AUTHOR_SIZE );

    ulong ts = blk ? blk->block_time : 0UL;
    TSDK_STORE( ulong, entry + OFFSET_TIMESTAMP, ts );

    ulong msg_sz = instruction_data_sz - 1UL;
    if( msg_sz >= MESSAGE_SIZE ) msg_sz = MESSAGE_SIZE - 1UL;
    memcpy( entry + OFFSET_TEXT, instr + 1, msg_sz );
    *( entry + OFFSET_TEXT + msg_sz ) = '\0';

    tsdk_printf( "POST: slot %lu, fee collected, ts=%lu\n", idx, ts );

  /* ── CMD_READ ─────────────────────────────────────────────────── */
  } else if( command == CMD_READ ) {
    TSDK_ASSERT_OR_REVERT( instruction_data_sz >= 1UL + sizeof(ulong), 12UL );

    ulong idx = TSDK_LOAD( ulong, instr + 1 );
    TSDK_ASSERT_OR_REVERT( idx < count, 13UL );

    uchar const * entry = entries_start + idx * ENTRY_SIZE;
    ulong ts = TSDK_LOAD( ulong, entry + OFFSET_TIMESTAMP );
    tsdk_printf( "READ: slot %lu ts=%lu\n", idx, ts );
    tsdk_printf( "Text: %s\n", (char const *)(entry + OFFSET_TEXT) );

  /* ── CMD_COUNT ────────────────────────────────────────────────── */
  } else if( command == CMD_COUNT ) {
    tsdk_printf( "Board: %lu messages\n", count );

    /* Also log vault balance */
    tsdk_account_meta_t const * vault_meta = tsdk_get_account_meta( VAULT_ACC_IDX );
    if( vault_meta ) {
      tsdk_printf( "Vault balance: %lu\n", vault_meta->balance );
    }

  /* ── CMD_WITHDRAW ─────────────────────────────────────────────── */
  } else if( command == CMD_WITHDRAW ) {
    /* Only the owner can withdraw */
    TSDK_ASSERT_OR_REVERT(
      memcmp( owner_field, caller, OWNER_SIZE ) == 0, 14UL );

    tsdk_account_meta_t const * vault_meta = tsdk_get_account_meta( VAULT_ACC_IDX );
    TSDK_ASSERT_OR_REVERT( vault_meta && vault_meta->balance > 0UL, 15UL );

    rc = tsys_set_account_data_writable( VAULT_ACC_IDX );
    TSDK_ASSERT_OR_REVERT( rc == 0UL, 16UL );

    ulong amount = vault_meta->balance;

    /* Transfer full vault balance to board account (owner retrieves it) */
    rc = tsys_account_transfer( VAULT_ACC_IDX, BOARD_ACC_IDX, amount );
    TSDK_ASSERT_OR_REVERT( rc == 0UL, 17UL );

    tsdk_printf( "WITHDRAW: %lu tokens to owner\n", amount );

  } else {
    tsdk_return( 18UL );
  }

  tsdk_return( TSDK_SUCCESS );
}
