// Copyright (c) 2025 OceanBase.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.

const MAX_REPLAY_BYTES: usize = 1024 * 1024;

pub(crate) struct TokenizeReplay {
    command: Vec<u8>,
    response: Vec<u8>,
    sequence_offsets: [usize; 5],
}

impl TokenizeReplay {
    // Eligibility is established by the normal C++ expression plan. Here we
    // accept only its complete, successful one-column/one-row response.
    pub(crate) fn learn(command: &[u8], response: &[u8]) -> Option<Self> {
        if command.first() != Some(&3)
            || command.len().checked_add(response.len())? > MAX_REPLAY_BYTES
        {
            return None;
        }
        let mut at = 0usize;
        let mut sequence_offsets = [0; 5];
        for (i, offset) in sequence_offsets.iter_mut().enumerate() {
            let header = response.get(at..at.checked_add(4)?)?;
            let len = usize::from(header[0])
                | (usize::from(header[1]) << 8)
                | (usize::from(header[2]) << 16);
            let end = at.checked_add(4)?.checked_add(len)?;
            let payload = response.get(at + 4..end)?;
            if i == 0 && payload != [1] {
                return None;
            }
            if i == 2 || i == 4 {
                if payload.len() != 5 || payload[..3] != [0xfe, 0, 0] {
                    return None;
                }
                let status = u16::from_le_bytes([payload[3], payload[4]]);
                if status & 2 == 0 || status & 9 != 0 {
                    return None;
                }
            }
            if payload.is_empty() || payload[0] == 0xff {
                return None;
            }
            *offset = at + 3;
            at = end;
        }
        (at == response.len()).then(|| Self {
            command: command.to_vec(),
            response: response.to_vec(),
            sequence_offsets,
        })
    }

    pub(crate) fn matches(&self, command: &[u8]) -> bool {
        self.command == command
    }

    pub(crate) fn write_response(&self, out: &mut Vec<u8>, mut sequence: u8) -> u8 {
        out.clear();
        out.extend_from_slice(&self.response);
        for &offset in &self.sequence_offsets {
            out[offset] = sequence;
            sequence = sequence.wrapping_add(1);
        }
        sequence
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn response() -> Vec<u8> {
        let mut bytes = Vec::new();
        for (i, payload) in [
            &[1][..],
            &[3, b'd', b'e', b'f'][..],
            &[0xfe, 0, 0, 2, 0][..],
            &[2, b'[', b']'][..],
            &[0xfe, 0, 0, 2, 0][..],
        ]
        .iter()
        .enumerate()
        {
            bytes.extend_from_slice(&[payload.len() as u8, 0, 0, i as u8 + 1]);
            bytes.extend_from_slice(payload);
        }
        bytes
    }

    #[test]
    fn learns_actual_bytes_and_reframes_sequence() {
        let command = b"\x03select tokenize('different text', 'ik')";
        let original = response();
        let replay = TokenizeReplay::learn(command, &original).unwrap();
        assert!(replay.matches(command));
        assert!(!replay.matches(b"\x03select tokenize('other text', 'ik')"));
        let mut out = Vec::new();
        assert_eq!(replay.write_response(&mut out, 254), 3);
        let mut at = 0;
        let mut seq = 254u8;
        while at < out.len() {
            assert_eq!(out[at + 3], seq);
            let len = out[at] as usize;
            assert_eq!(&out[at + 4..at + 4 + len], &original[at + 4..at + 4 + len]);
            at += 4 + len;
            seq = seq.wrapping_add(1);
        }
    }

    #[test]
    fn does_not_learn_partial_error_or_transaction_response() {
        let command = b"\x03select tokenize('text', 'beng')";
        let original = response();
        assert!(TokenizeReplay::learn(command, &original[..original.len() - 1]).is_none());
        assert!(TokenizeReplay::learn(b"\x16select tokenize('text', 'beng')", &original).is_none());
        let mut warned = original.clone();
        let end = warned.len();
        warned[end - 4] = 1;
        assert!(TokenizeReplay::learn(command, &warned).is_none());
        let mut transaction = original;
        let end = transaction.len();
        transaction[end - 2] = 3;
        assert!(TokenizeReplay::learn(command, &transaction).is_none());
    }
}
