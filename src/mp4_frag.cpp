#include "mp4_frag.hpp"

#include <cstring>

namespace {

void append_be32(std::string& out, uint32_t v) {
  out.push_back((v >> 24) & 0xFF);
  out.push_back((v >> 16) & 0xFF);
  out.push_back((v >> 8) & 0xFF);
  out.push_back(v & 0xFF);
}

void append_be64(std::string& out, uint64_t v) {
  out.push_back((v >> 56) & 0xFF);
  out.push_back((v >> 48) & 0xFF);
  out.push_back((v >> 40) & 0xFF);
  out.push_back((v >> 32) & 0xFF);
  out.push_back((v >> 24) & 0xFF);
  out.push_back((v >> 16) & 0xFF);
  out.push_back((v >> 8) & 0xFF);
  out.push_back(v & 0xFF);
}

void append_be16(std::string& out, uint16_t v) {
  out.push_back((v >> 8) & 0xFF);
  out.push_back(v & 0xFF);
}

void append_tag(std::string& out, const char* tag) {
  out.append(tag, tag + 4);
}

void append_box(std::string& out, const std::string& payload, const char* type) {
  append_be32(out, static_cast<uint32_t>(payload.size() + 8));
  append_tag(out, type);
  out.append(payload);
}

void append_version_flags(std::string& out, uint8_t version, uint32_t flags) {
  out.push_back(static_cast<char>(version));
  out.push_back(static_cast<char>((flags >> 16) & 0xFF));
  out.push_back(static_cast<char>((flags >> 8) & 0xFF));
  out.push_back(static_cast<char>(flags & 0xFF));
}

} // namespace

Mp4Fragmenter::Mp4Fragmenter(int width, int height, int fps, const std::vector<uint8_t>& sps, const std::vector<uint8_t>& pps)
    : width_(width), height_(height), fps_(fps), timescale_(90000), sps_(sps), pps_(pps) {}

std::string Mp4Fragmenter::build_init_segment() const {
  std::string out;

  // ftyp
  {
    std::string p;
    append_tag(p, "isom");
    append_be32(p, 0x00000200);
    append_tag(p, "isom");
    append_tag(p, "iso6");
    append_tag(p, "avc1");
    append_box(out, p, "ftyp");
  }

  // moov
  std::string moov;
  // mvhd
  {
    std::string p;
    append_version_flags(p, 0, 0);
    append_be32(p, 0); // creation time
    append_be32(p, 0); // modification
    append_be32(p, timescale_);
    append_be32(p, timescale_ * 60); // duration placeholder
    append_be32(p, 0x00010000); // rate 1.0
    append_be16(p, 0x0100);     // volume 1.0
    p.append(10, 0);            // reserved
    // matrix
    uint32_t matrix[9] = {0x00010000, 0, 0, 0, 0x00010000, 0, 0, 0, 0x40000000};
    for (auto m : matrix) append_be32(p, m);
    p.append(24, 0); // pre_defined
    append_be32(p, 2); // next_track_ID
    append_box(moov, p, "mvhd");
  }

  // trak
  std::string trak;
  // tkhd
  {
    std::string p;
    append_version_flags(p, 0, 0x000007); // enabled, in movie, in preview
    append_be32(p, 0); // creation
    append_be32(p, 0); // modification
    append_be32(p, 1); // track id
    append_be32(p, 0); // reserved
    append_be32(p, timescale_ * 60); // duration placeholder
    append_be64(p, 0); // reserved
    append_be16(p, 0); // layer
    append_be16(p, 0); // alternate group
    append_be16(p, 0x0000); // volume (0 for video)
    append_be16(p, 0);
    uint32_t matrix[9] = {0x00010000, 0, 0, 0, 0x00010000, 0, 0, 0, 0x40000000};
    for (auto m : matrix) append_be32(p, m);
    append_be32(p, static_cast<uint32_t>(width_) << 16);
    append_be32(p, static_cast<uint32_t>(height_) << 16);
    append_box(trak, p, "tkhd");
  }

  // mdia
  std::string mdia;
  // mdhd
  {
    std::string p;
    append_version_flags(p, 0, 0);
    append_be32(p, 0); // creation
    append_be32(p, 0); // modification
    append_be32(p, timescale_);
    append_be32(p, timescale_ * 60); // duration placeholder
    append_be16(p, 0x55c4); // lang und
    append_be16(p, 0);
    append_box(mdia, p, "mdhd");
  }
  // hdlr
  {
    std::string p;
    append_version_flags(p, 0, 0);
    append_be32(p, 0);
    append_tag(p, "vide");
    p.append(12, 0);
    p.push_back('v'); p.push_back('i'); p.push_back('d'); p.push_back('e');
    p.push_back('o'); p.push_back(0);
    append_box(mdia, p, "hdlr");
  }
  // minf
  std::string minf;
  // vmhd
  {
    std::string p;
    append_version_flags(p, 0, 0x000001);
    append_be16(p, 0); append_be16(p, 0); append_be16(p,0); append_be16(p,0);
    append_box(minf, p, "vmhd");
  }
  // dinf
  {
    std::string url;
    append_version_flags(url, 0, 0x000001); // self-contained
    std::string url_box;
    append_box(url_box, url, "url ");

    std::string dref_payload;
    append_version_flags(dref_payload, 0, 0);
    append_be32(dref_payload, 1);
    dref_payload.append(url_box);

    std::string dref_box;
    append_box(dref_box, dref_payload, "dref");

    std::string dinf_payload;
    dinf_payload.append(dref_box);
    append_box(minf, dinf_payload, "dinf");
  }
  // stbl
  std::string stbl;
  // stsd
  {
    std::string avc1;
    avc1.append(6, 0); // reserved
    append_be16(avc1, 1); // data ref index
    avc1.append(16, 0); // pre_defined + reserved
    append_be16(avc1, static_cast<uint16_t>(width_));
    append_be16(avc1, static_cast<uint16_t>(height_));
    append_be32(avc1, 0x00480000); // horiz resolution 72dpi
    append_be32(avc1, 0x00480000); // vert resolution
    append_be32(avc1, 0); // reserved
    append_be16(avc1, 1); // frame count
    avc1.append(32, 0); // compressorname
    append_be16(avc1, 0x0018); // depth
    append_be16(avc1, 0xffff); // pre-defined

    // avcC
    std::string avcc;
    avcc.push_back(1); // configurationVersion
    avcc.push_back(sps_.size() >= 4 ? (sps_[1]) : 0); // AVCProfileIndication
    avcc.push_back(sps_.size() >= 4 ? (sps_[2]) : 0); // profile_compat
    avcc.push_back(sps_.size() >= 4 ? (sps_[3]) : 0); // level
    avcc.push_back(0xFF); // lengthSizeMinusOne = 3 (4-byte lengths)
    avcc.push_back(0xE1); // numOfSequenceParameterSets =1
    append_be16(avcc, static_cast<uint16_t>(sps_.size()));
    avcc.append(reinterpret_cast<const char*>(sps_.data()), sps_.size());
    avcc.push_back(1); // numOfPictureParameterSets
    append_be16(avcc, static_cast<uint16_t>(pps_.size()));
    avcc.append(reinterpret_cast<const char*>(pps_.data()), pps_.size());

    append_box(avc1, avcc, "avcC");
    append_box(stbl, avc1, "avc1");

    std::string stsd_payload;
    append_version_flags(stsd_payload, 0, 0);
    append_be32(stsd_payload, 1);
    stsd_payload.append(stbl);
    stbl.clear();
    append_box(stbl, stsd_payload, "stsd");
  }
  // stts, stsc, stsz, stco empty
  {
    std::string p;
    append_version_flags(p, 0, 0);
    append_be32(p,0);
    append_box(stbl,p,"stts");
  }
  {
    std::string p;
    append_version_flags(p, 0, 0);
    append_be32(p,0);
    append_box(stbl,p,"stsc");
  }
  {
    std::string p;
    append_version_flags(p, 0, 0);
    append_be32(p, 0); // sample_size
    append_be32(p, 0); // sample_count
    append_box(stbl,p,"stsz");
  }
  {
    std::string p;
    append_version_flags(p, 0, 0);
    append_be32(p,0);
    append_box(stbl,p,"stco");
  }

  append_box(minf, stbl, "stbl");
  append_box(mdia, minf, "minf");
  append_box(trak, mdia, "mdia");
  append_box(moov, trak, "trak");

  // mvex/trex
  {
    std::string mvex;
    std::string trex;
    append_version_flags(trex, 0, 0);
    append_be32(trex, 1); // track id
    append_be32(trex, 1); // default sample desc (1-based)
    append_be32(trex, 0); // duration
    append_be32(trex, 0); // size
    append_be32(trex, 0x01000000); // flags
    append_box(mvex, trex, "trex");
    append_box(moov, mvex, "mvex");
  }

  append_box(out, moov, "moov");
  return out;
}

std::string Mp4Fragmenter::build_fragment(const std::vector<uint8_t>& avcc_sample, uint32_t seq,
                                          uint64_t base_decode_time, uint32_t sample_duration,
                                          bool keyframe) const {
  // Build sub-boxes
  std::string mfhd;
  {
    std::string p;
    append_version_flags(p, 0, 0);
    append_be32(p, seq);
    append_box(mfhd, p, "mfhd");
  }

  std::string tfhd;
  {
    std::string p;
    append_version_flags(p, 0, 0x020000); // default-base-is-moof
    append_be32(p, 1); // track id
    append_box(tfhd, p, "tfhd");
  }

  std::string tfdt;
  {
    std::string p;
    append_version_flags(p, 0, 0);
    append_be32(p, static_cast<uint32_t>(base_decode_time));
    append_box(tfdt, p, "tfdt");
  }

  // Precompute sizes to set trun data-offset (traf/moof include headers).
  uint32_t trun_payload =
      4 /*ver/flags*/ + 4 /*count*/ + 4 /*offset*/ + 4 /*duration*/ +
      4 /*size*/ + 4 /*flags*/;
  uint32_t trun_size = trun_payload + 8;
  uint32_t traf_size =
      static_cast<uint32_t>(tfhd.size() + tfdt.size() + trun_size + 8);
  uint32_t moof_size =
      static_cast<uint32_t>(mfhd.size() + traf_size + 8);
  uint32_t data_offset = moof_size + 8; // mdat header

  std::string trun;
  {
    std::string p;
    append_version_flags(p, 0, 0x000701);
    append_be32(p, 1);
    append_be32(p, data_offset);
    append_be32(p, sample_duration);
    append_be32(p, static_cast<uint32_t>(avcc_sample.size()));
    uint32_t flags = keyframe ? 0x02000000 : 0x01010000;
    append_be32(p, flags);
    append_box(trun, p, "trun");
  }

  std::string traf_payload;
  traf_payload.append(tfhd);
  traf_payload.append(tfdt);
  traf_payload.append(trun);
  std::string traf;
  append_box(traf, traf_payload, "traf");

  std::string moof_payload;
  moof_payload.append(mfhd);
  moof_payload.append(traf);
  std::string moof;
  append_box(moof, moof_payload, "moof");

  std::string out;
  out.append(moof);
  append_be32(out, static_cast<uint32_t>(8 + avcc_sample.size()));
  append_tag(out, "mdat");
  out.append(reinterpret_cast<const char*>(avcc_sample.data()), avcc_sample.size());
  return out;
}
