"""A miniature CJK language pack for the tests: a few Unifont-style glyphs and Unihan readings."""
import gzip, io, sys, zipfile

out = sys.argv[1]
chars = "한글日本中国你好人"
lines = []
for i, ch in enumerate(chars):
    rows = [(0x7ffe if r in (1, 14) or (r % 4 == i % 4) else 0x4002) for r in range(16)]
    lines.append("%04X:%s" % (ord(ch), "".join("%04X" % r for r in rows)))
lines.append("0041:" + "00" * 16)  # below the range kept: ignored
lines.append("3042:" + "%02X" % 0x3c * 16)  # あ, 8 wide
with gzip.open(out + "/unifont.hex.gz", "wb") as f:
    f.write(("\n".join(sorted(lines)) + "\n").encode())

readings = """# Unihan_Readings.txt (excerpt)
U+4E2D\tkHanyuPinlu\tzhōng(4875) zhòng(240)
U+4E2D\tkJapaneseKun\tNAKA UCHI ATARU
U+4E2D\tkJapaneseOn\tCHUU
U+4E2D\tkMandarin\tzhōng
U+4EBA\tkHanyuPinlu\trén(10000)
U+4EBA\tkJapaneseKun\tHITO
U+4EBA\tkJapaneseOn\tJIN NIN
U+4F60\tkHanyuPinlu\tnǐ(3000)
U+5973\tkHanyuPinlu\tnǚ(900)
U+56FD\tkHanyuPinlu\tguó(2000)
U+56FD\tkJapaneseKun\tKUNI
U+56FD\tkJapaneseOn\tKOKU
U+597D\tkHanyuPinlu\thǎo(2500) hào(300)
U+5FE0\tkTGHZ2013\t487.050:zhōng
U+65E5\tkJapaneseKun\tHI KA
U+65E5\tkJapaneseOn\tNICHI JITSU
U+672C\tkJapaneseKun\tMOTO
U+672C\tkJapaneseOn\tHON
U+7A2E\tkJapaneseOn\tSHU
U+8846\tkHanyuPinlu\tzhòng(100)
"""
other = """# Unihan_OtherMappings.txt (excerpt)
U+4E2D\tkJis0\t3570
U+4EBA\tkJis0\t3145
U+56FD\tkJis0\t2581
U+65E5\tkJis0\t3892
U+672C\tkJis0\t4360
U+7A2E\tkJis0\t2879
"""
with zipfile.ZipFile(out + "/Unihan.zip", "w", zipfile.ZIP_DEFLATED) as z:
    z.writestr("Unihan_Readings.txt", readings)
    z.writestr("Unihan_OtherMappings.txt", other)
