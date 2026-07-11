/* thru-kvstore - Thru Program
 * A simple on-chain key-value store
 * Stores up to 16 fixed-size (32-byte key, 32-byte value) entries
 *
 * Instruction data layout:
 *   byte 0:      command (0 = write, 1 = read)
 *   bytes 1-32:  key (32 bytes)
 *   bytes 33-64: value (32 bytes, write only)
 */
#include <thru-sdk/c/tn_sdk.h>
#include <thru-sdk/c/tn_sdk_syscall.h>

/* Commands */
#define CMD_WRITE (0U)
#define CMD_READ  (1U)

/* Limits */
#define MAX_ENTRIES     (16UL)
#define KEY_SIZE        (32UL)
#define VALUE_SIZE      (32UL)
#define ENTRY_SIZE      (KEY_SIZE + VALUE_SIZE)
#define HEADER_SIZE     (sizeof(ulong))
#define MAX_DATA_SIZE   (HEADER_SIZE + MAX_ENTRIES * ENTRY_SIZE)

/* Minimum instruction sizes */
#define MIN_INSTR_READ  (1UL + KEY_SIZE)
#define MIN_INSTR_WRITE (1UL + KEY_SIZE + VALUE_SIZE)

/* Find an entry by key — returns index if found, MAX_ENTRIES if not */
static ulong
find_entry( uchar const * data, ulong count, uchar const * key ) {
  for( ulong i = 0UL; i < count; i++ ) {
    uchar const * entry_key = data + HEADER_SIZE + i * ENTRY_SIZE;
    if( memcmp( entry_key, key, KEY_SIZE ) == 0 ) {
      return i;
    }
  }
  return MAX_ENTRIES; /* not found */
}

TSDK_ENTRYPOINT_FN void
start( void const * instruction_data,
       ulong        instruction_data_sz ) {

  /* Validate instruction data exists */
  TSDK_ASSERT_OR_REVERT( instruction_data != NULL, 1UL );
  TSDK_ASSERT_OR_REVERT( instruction_data_sz >= 1UL, 2UL );

  uchar const * instr = (uchar const *)instruction_data;
  uchar command = instr[0];

  /* Account 0 = the KV store storage account */
  ushort const store_acc_idx = 0;

  /* Ensure program owns this account */
  TSDK_ASSERT_OR_REVERT(
    tsdk_is_account_owned_by_current_program( store_acc_idx ), 3UL );

  /* Make account data writable */
  ulong rc = tsys_set_account_data_writable( store_acc_idx );
  TSDK_ASSERT_OR_REVERT( rc == 0UL, 4UL );

  /* Get account data pointer and metadata */
  void * raw_data = tsdk_get_account_data_ptr( store_acc_idx );
  tsdk_account_meta_t const * meta = tsdk_get_account_meta( store_acc_idx );

  ulong count = 0UL;

  /* Initialize account if first use */
  if( !meta || meta->data_sz < HEADER_SIZE ) {
    rc = tsys_account_resize( store_acc_idx, MAX_DATA_SIZE );
    TSDK_ASSERT_OR_REVERT( rc == 0UL, 5UL );
    raw_data = tsdk_get_account_data_ptr( store_acc_idx );
    TSDK_STORE( ulong, raw_data, 0UL );
  } else {
    count = TSDK_LOAD( ulong, raw_data );
  }

  uchar * data = (uchar *)raw_data;

  if( command == CMD_WRITE ) {
    /* Validate instruction length */
    TSDK_ASSERT_OR_REVERT( instruction_data_sz >= MIN_INSTR_WRITE, 6UL );

    uchar const * key   = instr + 1;
    uchar const * value = instr + 1 + KEY_SIZE;

    /* Check if key already exists — update it */
    ulong idx = find_entry( data, count, key );

    if( idx == MAX_ENTRIES ) {
      /* New entry — check we have space */
      TSDK_ASSERT_OR_REVERT( count < MAX_ENTRIES, 7UL );
      idx = count;
      count++;
      TSDK_STORE( ulong, data, count );
    }

    /* Write key and value */
    uchar * entry = data + HEADER_SIZE + idx * ENTRY_SIZE;
    memcpy( entry,            key,   KEY_SIZE   );
    memcpy( entry + KEY_SIZE, value, VALUE_SIZE );

    tsdk_printf( "KV write: entry %lu of %lu\n", idx, count );

  } else if( command == CMD_READ ) {
    /* Validate instruction length */
    TSDK_ASSERT_OR_REVERT( instruction_data_sz >= MIN_INSTR_READ, 8UL );

    uchar const * key = instr + 1;

    ulong idx = find_entry( data, count, key );
    TSDK_ASSERT_OR_REVERT( idx != MAX_ENTRIES, 9UL ); /* key not found */

    uchar const * value = data + HEADER_SIZE + idx * ENTRY_SIZE + KEY_SIZE;
    tsdk_printf( "KV read: found entry %lu\n", idx );

    /* Log first 8 bytes of value as a ulong for visibility */
    ulong val_preview = TSDK_LOAD( ulong, value );
    tsdk_printf( "Value preview (first 8 bytes): %lu\n", val_preview );

  } else {
    /* Unknown command */
    tsdk_return( 10UL );
  }

  tsdk_return( TSDK_SUCCESS );
}