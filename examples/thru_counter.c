/* thru-counter - Thru Program
 * Interactive counter — caller passes increment amount via instruction data
 * If no instruction data provided, defaults to incrementing by 1
 */
#include <thru-sdk/c/tn_sdk.h>
#include <thru-sdk/c/tn_sdk_syscall.h>

TSDK_ENTRYPOINT_FN void
start( void const * instruction_data,
       ulong        instruction_data_sz ) {

  /* Account 0 = the counter storage account */
  ushort const counter_acc_idx = 0;

  /* Ensure the program owns this account */
  TSDK_ASSERT_OR_REVERT(
    tsdk_is_account_owned_by_current_program( counter_acc_idx ), 1UL );

  /* Make account data writable */
  ulong rc = tsys_set_account_data_writable( counter_acc_idx );
  TSDK_ASSERT_OR_REVERT( rc == 0UL, 2UL );

  /* Read increment amount from instruction data
     Caller passes a ulong (8 bytes) as the amount to increment by.
     If nothing is passed, default to 1 */
  ulong increment = 1UL;
  if( instruction_data && instruction_data_sz >= sizeof(ulong) ) {
    increment = TSDK_LOAD( ulong, instruction_data );
    /* Sanity cap — reject increments of 0 or unreasonably large values */
    TSDK_ASSERT_OR_REVERT( increment > 0UL, 4UL );
    TSDK_ASSERT_OR_REVERT( increment <= 1000000UL, 5UL );
  }

  /* Get pointer to account data */
  void * data = tsdk_get_account_data_ptr( counter_acc_idx );
  tsdk_account_meta_t const * meta = tsdk_get_account_meta( counter_acc_idx );

  ulong count = 0UL;

  if( meta && meta->data_sz >= sizeof(ulong) ) {
    /* Load existing count safely */
    count = TSDK_LOAD( ulong, data );
  } else {
    /* First call — resize account to hold one ulong */
    rc = tsys_account_resize( counter_acc_idx, sizeof(ulong) );
    TSDK_ASSERT_OR_REVERT( rc == 0UL, 3UL );
    data = tsdk_get_account_data_ptr( counter_acc_idx );
  }

  /* Add increment and store */
  count += increment;
  TSDK_STORE( ulong, data, count );

  /* Log result */
  tsdk_printf( "Counter: %lu (added %lu)\n", count, increment );

  tsdk_return( TSDK_SUCCESS );
}