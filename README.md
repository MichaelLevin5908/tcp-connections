# Description of Work (10 points)

This is new for Project 1. We ask you to include a file called `README.md` that contains a quick description of:

1. the design choices you made,
2. any problems you encountered while creating your solution, and
3. your solutions to those problems.

This is not meant to be comprehensive; less than 300 words will suffice.

## Description of Work

**Design choices.** The transport module now keeps distinct paths for data and pure ACK generation while preserving a sliding-window protocol. Send and receive queues remain linked lists so selective retransmissions and late in-order delivery are straightforward. Timers are single-shot and tied to the oldest outstanding segment, but duplicate-ACK triggers allow a fast retransmit of the base packet. Sequence numbers advance per segment to stay compatible with the provided harness.

**Problems.** Early versions left stale packets in the send queue when an ACK arrived, so the window never reopened. Buffered out-of-order data was also written back out with a single-pass scan, causing missed contiguous deliveries. Finally, the main loop sometimes skipped pure ACKs when no outgoing payloads were pending.

**Solutions.** ACK handling now walks the send list with pointer-to-pointer updates, freeing acknowledged buffers and refreshing `base_pkt`. Receive reassembly repeats until no buffered segment matches the next expected sequence, ensuring the advertised window shrinks only while data is queued. The listen loop was reordered to flush pure ACKs immediately and to centralize timer resets, which resolved the intermittent handshake failure.

**Notes.** Remaining tasks include tuning the retransmission timeout (currently fixed) and adding integration tests that simulate large back-to-back transfers to confirm the fast-retransmit path.