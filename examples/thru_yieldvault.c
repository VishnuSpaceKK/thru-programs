/* thru-yieldvault - Thru Program
 * A simple deposit/yield vault
 *
 * Users deposit tokens, earn 1% yield per 1000 slots, withdraw anytime.
 * Owner can view total vault stats.
 *
 * Accounts:
 *   0 = vault storage (owner + user entries)
 *   1 = vault token account (holds deposited tokens)
 *
 * Storage layout (account 0):
 *   [32 bytes: owner pubkey]
 *   [8 bytes:  user count]
 *   [48 bytes x 16: entries]
 *     entry: [32B pubkey | 8B amount | 8B deposit_slot]
 *   Total: 32 + 8 + 16*48 = 808 bytes
 *
 * Instruction data:
 *   byte 0: command
 *     0 = deposit   (bytes 1-8: amount as ulong)
 *     1 = withdraw  (bytes 1-8: amount to withdraw as ulong)
 *     2 = balance   (check your own deposited balance)
 *     3 = stats     (owner only: total deposits + user count)
 */
#include <thru-sdk/c/tn_sdk.h>
#include <thru-sdk/c/tn_sdk_syscall.h>

/* Commands */
#define CMD_DEPOSIT  (0U)
#define CMD_WITHDRAW (1U)
#define CMD_BALANCE  (2U)
#define CMD_STATS    (3U)

/* Layout */
#define OWNER_SIZE    (32UL)
#define COUNT_SIZE    (sizeof(ulong))
#define HEADER_SIZE   (OWNER_SIZE + COUNT_SIZE)
#define MAX_USERS     (16UL)
#define PUBKEY_SIZE   (32UL)
#define AMOUNT_SIZE   (sizeof(ulong))
#define SLOT_SIZE     (sizeof(ulong))
#define ENTRY_SIZE    (PUBKEY_SIZE + AMOUNT_SIZE + SLOT_SIZE)
#define MAX_DATA_SIZE (HEADER_SIZE + MAX_USERS * ENTRY_SIZE)

/* Entry field offsets */
#define OFFSET_PUBKEY (0UL)
#define OFFSET_AMOUNT (PUBKEY_SIZE)
#define OFFSET_SLOT   (PUBKEY_SIZE + AMOUNT_SIZE)

/* Yield: 1% per 1000 slots elapsed since deposit */
#define YIELD_SLOTS   (1000UL)
#define YIELD_PCT     (1UL)

/* Account indices */
#define STORE_ACC_IDX (0U)
#define VAULT_ACC_IDX (1U)

/* Find user entry by pubkey */
static ulong
find_user( uchar const * entries, ulong count, uchar const * pubkey ) {
  for( ulong i = 0UL; i < count; i++ ) {
    if( memcmp( entries + i * ENTRY_SIZE + OFFSET_PUBKEY,
                pubkey, PUBKEY_SIZE ) == 0 ) return i;
  }
  return MAX_USERS;
}

/* Calculate yield: 1% per 1000 slots, integer math only */
static ulong
calc_yield( ulong amount, ulong deposit_slot, ulong current_slot ) {
  if( current_slot <= deposit_slot ) return 0UL;
  ulong slots_elapsed = current_slot - deposit_slot;
  ulong periods = slots_elapsed / YIELD_SLOTS;
  return ( amount * YIELD_PCT * periods ) / 100UL;
}

TSDK_ENTRYPOINT_FN void
start( void const * instruction_data,
       ulong        instruction_data_sz ) {

  TSDK_ASSERT_OR_REVERT( instruction_data != NULL, 1UL );
  TSDK_ASSERT_OR_REVERT( instruction_data_sz >= 1UL, 2UL );

  uchar const * instr   = (uchar const *)instruction_data;
  uchar         command = instr[0];

  /* Both accounts must be owned by this program */
  TSDK_ASSERT_OR_REVERT(
    tsdk_is_account_owned_by_current_program( STORE_ACC_IDX ), 3UL );
  TSDK_ASSERT_OR_REVERT(
    tsdk_is_account_owned_by_current_program( VAULT_ACC_IDX ), 4UL );

  tsdk_txn_t       const * txn = tsdk_get_txn();
  tsdk_block_ctx_t const * blk = tsdk_get_current_block_ctx();
  uchar            const * caller = txn->hdr.v1.fee_payer_pubkey.key;
  ulong                    current_slot = blk ? blk->slot : 0UL;

  /* Make store writable */
  ulong rc = tsys_set_account_data_writable( STORE_ACC_IDX );
  TSDK_ASSERT_OR_REVERT( rc == 0UL, 5UL );

  void * raw_data = tsdk_get_account_data_ptr( STORE_ACC_IDX );
  tsdk_account_meta_t const * meta = tsdk_get_account_meta( STORE_ACC_IDX );

  /* Initialize on first use */
  if( !meta || meta->data_sz < HEADER_SIZE ) {
    rc = tsys_account_resize( STORE_ACC_IDX, MAX_DATA_SIZE );
    TSDK_ASSERT_OR_REVERT( rc == 0UL, 6UL );
    raw_data = tsdk_get_account_data_ptr( STORE_ACC_IDX );
    memcpy( raw_data, caller, OWNER_SIZE );
    TSDK_STORE( ulong, (uchar *)raw_data + OWNER_SIZE, 0UL );
  }

  uchar * data          = (uchar *)raw_data;
  uchar * owner_field   = data;
  uchar * count_field   = data + OWNER_SIZE;
  uchar * entries       = data + HEADER_SIZE;
  ulong   count         = TSDK_LOAD( ulong, count_field );

  /* ── CMD_DEPOSIT ──────────────────────────────────────────────── */
  if( command == CMD_DEPOSIT ) {
    TSDK_ASSERT_OR_REVERT( instruction_data_sz >= 1UL + sizeof(ulong), 7UL );

    ulong amount = TSDK_LOAD( ulong, instr + 1 );
    TSDK_ASSERT_OR_REVERT( amount > 0UL, 8UL );

    /* Check caller has enough balance */
    tsdk_account_meta_t const * store_meta =
      tsdk_get_account_meta( STORE_ACC_IDX );
    TSDK_ASSERT_OR_REVERT(
      store_meta && store_meta->balance >= amount, 9UL );

    /* Transfer tokens from store account to vault */
    rc = tsys_set_account_data_writable( VAULT_ACC_IDX );
    TSDK_ASSERT_OR_REVERT( rc == 0UL, 10UL );

    rc = tsys_account_transfer( STORE_ACC_IDX, VAULT_ACC_IDX, amount );
    TSDK_ASSERT_OR_REVERT( rc == 0UL, 11UL );

    /* Find or create user entry */
    ulong idx = find_user( entries, count, caller );
    if( idx == MAX_USERS ) {
      TSDK_ASSERT_OR_REVERT( count < MAX_USERS, 12UL );
      idx = count++;
      TSDK_STORE( ulong, count_field, count );
    }

    uchar * entry = entries + idx * ENTRY_SIZE;

    /* If existing user — add to their balance, keep original deposit slot */
    ulong existing = TSDK_LOAD( ulong, entry + OFFSET_AMOUNT );
    memcpy( entry + OFFSET_PUBKEY, caller, PUBKEY_SIZE );
    TSDK_STORE( ulong, entry + OFFSET_AMOUNT, existing + amount );

    /* Only set deposit slot on first deposit */
    if( existing == 0UL ) {
      TSDK_STORE( ulong, entry + OFFSET_SLOT, current_slot );
    }

    tsdk_printf( "DEPOSIT: %lu tokens, slot=%lu, total=%lu\n",
                 amount, current_slot, existing + amount );

  /* ── CMD_WITHDRAW ─────────────────────────────────────────────── */
  } else if( command == CMD_WITHDRAW ) {
    TSDK_ASSERT_OR_REVERT( instruction_data_sz >= 1UL + sizeof(ulong), 13UL );

    ulong amount = TSDK_LOAD( ulong, instr + 1 );
    TSDK_ASSERT_OR_REVERT( amount > 0UL, 14UL );

    ulong idx = find_user( entries, count, caller );
    TSDK_ASSERT_OR_REVERT( idx != MAX_USERS, 15UL );

    uchar * entry        = entries + idx * ENTRY_SIZE;
    ulong   balance      = TSDK_LOAD( ulong, entry + OFFSET_AMOUNT );
    ulong   deposit_slot = TSDK_LOAD( ulong, entry + OFFSET_SLOT );

    TSDK_ASSERT_OR_REVERT( balance >= amount, 16UL );

    /* Calculate yield on withdrawn amount */
    ulong yield = calc_yield( amount, deposit_slot, current_slot );

    /* Check vault has enough for amount + yield */
    tsdk_account_meta_t const * vault_meta =
      tsdk_get_account_meta( VAULT_ACC_IDX );
    ulong total_out = amount + yield;
    TSDK_ASSERT_OR_REVERT(
      vault_meta && vault_meta->balance >= total_out, 17UL );

    /* Transfer from vault to store account */
    rc = tsys_set_account_data_writable( VAULT_ACC_IDX );
    TSDK_ASSERT_OR_REVERT( rc == 0UL, 18UL );

    rc = tsys_account_transfer( VAULT_ACC_IDX, STORE_ACC_IDX, total_out );
    TSDK_ASSERT_OR_REVERT( rc == 0UL, 19UL );

    /* Update user balance */
    TSDK_STORE( ulong, entry + OFFSET_AMOUNT, balance - amount );

    tsdk_printf( "WITHDRAW: %lu tokens + %lu yield, slot=%lu\n",
                 amount, yield, current_slot );

  /* ── CMD_BALANCE ──────────────────────────────────────────────── */
  } else if( command == CMD_BALANCE ) {
    ulong idx = find_user( entries, count, caller );

    if( idx == MAX_USERS ) {
      tsdk_printf( "BALANCE: no deposit found\n" );
    } else {
      uchar * entry        = entries + idx * ENTRY_SIZE;
      ulong   balance      = TSDK_LOAD( ulong, entry + OFFSET_AMOUNT );
      ulong   deposit_slot = TSDK_LOAD( ulong, entry + OFFSET_SLOT );
      ulong   yield        = calc_yield( balance, deposit_slot, current_slot );

      tsdk_printf( "BALANCE: %lu deposited, %lu yield accrued\n",
                   balance, yield );
    }

  /* ── CMD_STATS ────────────────────────────────────────────────── */
  } else if( command == CMD_STATS ) {
    TSDK_ASSERT_OR_REVERT(
      memcmp( owner_field, caller, OWNER_SIZE ) == 0, 20UL );

    tsdk_account_meta_t const * vault_meta =
      tsdk_get_account_meta( VAULT_ACC_IDX );
    ulong vault_balance = vault_meta ? vault_meta->balance : 0UL;

    tsdk_printf( "STATS: %lu users, vault=%lu tokens\n",
                 count, vault_balance );

  } else {
    tsdk_return( 21UL );
  }

  tsdk_return( TSDK_SUCCESS );
}
