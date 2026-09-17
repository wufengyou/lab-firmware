import unittest

from xbee_lab import LabPacket, crc16_ccitt, decode_lab_packet, encode_lab_packet, percentile


class ProtocolTests(unittest.TestCase):
    def test_known_crc_vector(self):
        self.assertEqual(crc16_ccitt(b"123456789"), 0x29B1)

    def test_packet_round_trip(self):
        encoded = encode_lab_packet("D", 42, "TEMP=25.6")
        self.assertEqual(decode_lab_packet(encoded), LabPacket("D", 42, "TEMP=25.6"))

    def test_corruption_is_rejected(self):
        encoded = bytearray(encode_lab_packet("D", 7, "OK"))
        encoded[5] ^= 1
        with self.assertRaisesRegex(RuntimeError, "CRC mismatch"):
            decode_lab_packet(bytes(encoded))

    def test_reserved_delimiters_are_rejected(self):
        with self.assertRaisesRegex(RuntimeError, "must not contain"):
            encode_lab_packet("D", 1, "bad|payload")

    def test_nearest_rank_percentile(self):
        self.assertEqual(percentile([1, 2, 3, 4, 100], 0.95), 100)


if __name__ == "__main__":
    unittest.main()
