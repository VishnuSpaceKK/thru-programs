/* thru-msgboard - Thru Program
 * An on-chain message board with author-based access control
 *
 * Each author (identified by their fee payer pubkey) gets one message slot.
 * Only the author can write or update their own message.
 * Anyone can read any message.
 *
 * Storage layout:
 *   [8 bytes:   message count (ulong)]
 *   [320 bytes: entry 0 — 32B author + 8B timestamp + 24B padding + 256B text]
 *   [320 bytes: entry 1 ...]
 *   ...up to 8 entries = 2,568 bytes total
 *
 * Instruction data:
 *   byte 0:     command (0=write, 1=read by index, 2=read count)
 *   write:      bytes 1-255 = message text (null-terminated)
 *   read:       bytes 1-8   = slot index as ulong
 */
#include <thru-sdk/c/tn_sdk.h>
#include <thru-sdk/c/tn_sdk_syscall.h>

/* Commands */
#define CMD_WRITE      (0U)
#define CMD_READ       (1U)
#define CMD_READ_COUNT (2U)

/* Layout constants */
#define MAX_MESSAGES   (8UL)
#define AUTHOR_SIZE    (32UL)
#define TIMESTAMP_SIZE (8UL)
#define PADDING_SIZE   (24UL)
#define MESSAGE_SIZE   (256UL)
#define ENTRY_SIZE     (AUTHOR_SIZE + TIMESTAMP_SIZE + PADDING_SIZE + MESSAGE_SIZE)
#define HEADER_SIZE    (sizeof(ulong))
#define MAX_DATA_SIZE  (HEADER_SIZE + MAX_MESSAGES * ENTRY_SIZE)

/* Entry field offsets within an entry */
#define OFFSET_AUTHOR    (0UL)
#define OFFSET_TIMESTAMP (AUTHOR_SIZE)
#define OFFSET_TEXT      (AUTHOR_SIZE + TIMESTAMP_SIZE + PADDING_SIZE)

/* Find an entry by author pubkey — returns index or MAX_MESSAGES if not found */
static ulong
find_by_author( uchar const * data, ulong count, uchar const * author ) {
  for( ulong i = 0UL; i < count; i++ ) {
    uchar const * entry_author = data + HEADER_SIZE + i * ENTRY_SIZE + OFFSET_AUTHOR;
    if( memcmp( entry_author, author, AUTHOR_SIZE ) == 0 ) {
      return i;
    }
  }
  return MAX_MESSAGES;
}

TSDK_ENTRYPOINT_FN void
start( void const * instruction_data,
       ulong        instruction_data_sz ) {

  /* Validate instruction data */
  TSDK_ASSERT_OR_REVERT( instruction_data != NULL, 1UL );
  TSDK_ASSERT_OR_REVERT( instruction_data_sz >= 1UL, 2UL );

  uchar const * instr = (uchar const *)instruction_data;
  uchar command = instr[0];

  /* Account 0 = the message board storage account */
  ushort const board_acc_idx = 0;

  /* Program must own this account */
  TSDK_ASSERT_OR_REVERT(
    tsdk_is_account_owned_by_current_program( board_acc_idx ), 3UL );

  /* Get transaction for author identity and block context for timestamp */
  tsdk_txn_t const * txn = tsdk_get_txn();
  tsdk_block_ctx_t const * blk = tsdk_get_current_block_ctx();

  /* Make account writable */
  ulong rc = tsys_set_account_data_writable( board_acc_idx );
  TSDK_ASSERT_OR_REVERT( rc == 0UL, 4UL );

  /* Get account data */
  void * raw_data = tsdk_get_account_data_ptr( board_acc_idx );
  tsdk_account_meta_t const * meta = tsdk_get_account_meta( board_acc_idx );

  ulong count = 0UL;

  /* Initialize on first use */
  if( !meta || meta->data_sz < HEADER_SIZE ) {
    rc = tsys_account_resize( board_acc_idx, MAX_DATA_SIZE );
    TSDK_ASSERT_OR_REVERT( rc == 0UL, 5UL );
    raw_data = tsdk_get_account_data_ptr( board_acc_idx );
    TSDK_STORE( ulong, raw_data, 0UL );
  } else {
    count = TSDK_LOAD( ulong, raw_data );
  }

  uchar * data = (uchar *)raw_data;

  if( command == CMD_WRITE ) {
    /* Need at least 2 bytes: command + at least 1 char of message */
    TSDK_ASSERT_OR_REVERT( instruction_data_sz >= 2UL, 6UL );

    /* Author = fee payer pubkey from the transaction */
    uchar const * author = txn->hdr.v1.fee_payer_pubkey.key;

    /* Check if this author already has a slot */
    ulong idx = find_by_author( data, count, author );

    if( idx == MAX_MESSAGES ) {
      /* New author — check space */
      TSDK_ASSERT_OR_REVERT( count < MAX_MESSAGES, 7UL );
      idx = count;
      count++;
      TSDK_STORE( ulong, data, count );
    }

    /* Write entry */
    uchar * entry = data + HEADER_SIZE + idx * ENTRY_SIZE;

    /* Author pubkey */
    memcpy( entry + OFFSET_AUTHOR, author, AUTHOR_SIZE );

    /* Timestamp from block context */
    ulong ts = blk ? blk->block_time : 0UL;
    TSDK_STORE( ulong, entry + OFFSET_TIMESTAMP, ts );

    /* Message text — copy up to MESSAGE_SIZE-1 bytes, always null-terminate */
    ulong msg_sz = instruction_data_sz - 1UL;
    if( msg_sz >= MESSAGE_SIZE ) msg_sz = MESSAGE_SIZE - 1UL;
    memcpy( entry + OFFSET_TEXT, instr + 1, msg_sz );
    *( entry + OFFSET_TEXT + msg_sz ) = '\0';

    tsdk_printf( "MSG write: slot %lu of %lu, ts=%lu\n", idx, count, ts );

  } else if( command == CMD_READ ) {
    /* Need command + 8-byte index */
    TSDK_ASSERT_OR_REVERT( instruction_data_sz >= 1UL + sizeof(ulong), 8UL );

    ulong idx = TSDK_LOAD( ulong, instr + 1 );
    TSDK_ASSERT_OR_REVERT( idx < count, 9UL );

    uchar const * entry = data + HEADER_SIZE + idx * ENTRY_SIZE;
    ulong ts = TSDK_LOAD( ulong, entry + OFFSET_TIMESTAMP );

    tsdk_printf( "MSG read: slot %lu, ts=%lu\n", idx, ts );
    /* Message text is at entry + OFFSET_TEXT, null-terminated */
    tsdk_printf( "Text: %s\n", (char const *)(entry + OFFSET_TEXT) );

  } else if( command == CMD_READ_COUNT ) {
    tsdk_printf( "MSG board: %lu messages\n", count );

  } else {
    tsdk_return( 10UL );
  }

  tsdk_return( TSDK_SUCCESS );
}
