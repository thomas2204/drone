# F103 responder receive-error recovery

This is a suggested change for the beacon owner. The F103 source folder has
not been changed. Radio configuration, packet contents and reply delay stay
the same.

In `Core/Src/beacon_twr.c`, inside `beacon_twr_step`, the branch
`if (!(status & SYS_STATUS_RXFCG_BIT_MASK))` currently writes only
`SYS_STATUS_RXFCG_BIT_MASK` while its comment says it clears error flags.
That write does not clear the receive error that caused the branch. Stale
errors can cause repeated receiver rearming and further missed polls.

Replace that branch with:

```c
if (!(status & SYS_STATUS_RXFCG_BIT_MASK)) {
    dwt_forcetrxoff();
    dwt_writesysstatuslo(SYS_STATUS_ALL_RX_ERR | SYS_STATUS_ALL_RX_TO);
    st.rx_err++;
    return;
}
```

The function already set `rx_armed = 0` before this branch, so the next pass
re-enables the receiver. Validate on the beacon hardware after rebuilding:
verify that valid polls and replies resume after a receive error, rather than
only seeing the error count increase.

To locate the failing direction, collect the beacon's `polls`, `replies`,
`rx_err`, `tx_err` and drone counters over the same 10-second interval:

- Few beacon polls: drone-to-beacon link or responder receiver recovery.
- Polls increasing but few replies, with tx_err increasing: responder scheduling.
- Replies increasing but few drone frames: beacon-to-drone link or drone reception.
- Drone BAD increasing: reply validation or distance calculation rejection.

This corrects a source-level bug, but does not prove that it caused the observed
one-metre limit. Antennas, cables, power and signal strength need checking too.
