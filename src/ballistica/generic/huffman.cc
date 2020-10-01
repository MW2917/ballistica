// Copyright (c) 2011-2020 Eric Froemling

#include "ballistica/generic/huffman.h"

#include <string>
#include <vector>

#include "ballistica/networking/networking.h"

namespace ballistica {

// Yes, I should clean this up to use unsigned vals, but it seems to work
// fine for now so I don't want to touch it.
#pragma clang diagnostic push
#pragma ide diagnostic ignored "hicpp-signed-bitwise"

// how much data we read in training mode before spitting out results
#if HUFFMAN_TRAINING_MODE
const int kTrainingLength = 200000;
#endif

// we currently just have a static table of char frequencies - this can be
// generated by setting "training mode" on.
static int g_freqs[] = {
    101342, 9667, 3497, 1072, 0, 3793, 0, 0, 2815, 5235, 0, 0, 0, 3570, 0, 0,
    0,      1383, 0,    0,    0, 2970, 0, 0, 2857, 0,    0, 0, 0, 0,    0, 0,
    0,      1199, 0,    0,    0, 0,    0, 0, 0,    0,    0, 0, 0, 0,    0, 0,
    0,      0,    0,    0,    0, 0,    0, 0, 0,    0,    0, 0, 0, 0,    0, 1494,
    1974,   0,    0,    0,    0, 0,    0, 0, 0,    0,    0, 0, 0, 1351, 0, 0,
    0,      0,    0,    0,    0, 0,    0, 0, 0,    0,    0, 0, 0, 0,    0, 0,
    0,      0,    0,    0,    0, 0,    0, 0, 0,    0,    0, 0, 0, 0,    0, 0,
    0,      0,    0,    0,    0, 0,    0, 0, 0,    0,    0, 0, 0, 0,    0, 0,
    0,      0,    0,    0,    0, 0,    0, 0, 0,    0,    0, 0, 0, 0,    0, 0,
    0,      0,    0,    0,    0, 0,    0, 0, 0,    0,    0, 0, 0, 0,    0, 0,
    0,      0,    0,    0,    0, 0,    0, 0, 0,    0,    0, 0, 0, 0,    0, 0,
    0,      0,    0,    0,    0, 0,    0, 0, 0,    0,    0, 0, 0, 0,    0, 1475,
    0,      0,    0,    0,    0, 0,    0, 0, 0,    0,    0, 0, 0, 0,    0, 0,
    0,      0,    0,    0,    0, 0,    0, 0, 0,    0,    0, 0, 0, 0,    0, 0,
    0,      0,    0,    0,    0, 0,    0, 0, 0,    0,    0, 0, 0, 0,    0, 0,
    0,      0,    0,    0,    0, 0,    0, 0, 0,    0,    0, 0, 0, 0,    0, 0};

static void DoWriteBits(char** ptr, int* bit, int val, int val_bits) {
  int src_bit = 0;
  while (src_bit < val_bits) {
    **ptr |= ((val >> src_bit) & 0x01) << (*bit);  // NOLINT
    if ((*bit) == 7) (*ptr)++;
    (*bit) = ((*bit) + 1) % 8;
    src_bit++;
  }
}

Huffman::Huffman() : built(false) {
  static_assert(sizeof(g_freqs) == sizeof(int) * 256);
  build();
}

Huffman::~Huffman() = default;

auto Huffman::compress(const std::vector<uint8_t>& src)
    -> std::vector<uint8_t> {
#if BA_HUFFMAN_NET_COMPRESSION

  auto length = static_cast<uint32_t>(src.size());
  const char* data = (const char*)src.data();

  // IMPORTANT:
  // our uncompressed packets have a type byte at the beginning
  // (which should just be a few bits)
  // and the compressed ones have a remainder byte (4 bits of which are used)
  // ...so the first few bits should always be unused.
  // we hijack the highest bit to denote whether we're sending
  // a compressed or uncompressed packet (1 for compressed, 0 for uncompressed)
  BA_PRECONDITION(data[0] >> 7 == 0);

  // see how many bits we'll need
  uint32_t bit_count = 0;
  for (uint32_t i = 0; i < length; i++) {
    bit_count += nodes_[static_cast<uint8_t>(data[i])].bits;
  }

  // round up to next byte and add our one-byte header
  uint32_t length_out = bit_count / 8 + 1;
  if (bit_count % 8) {
    length_out++;
  }
  bit_count %= 8;

  // if compressed is bigger than uncompressed, go with uncompressed - just
  // return the data they provided
  if ((length_out >= length)) {
    return src;
  } else {
    std::vector<uint8_t> out(length_out, 0);

    // first byte gives our number of empty trailing bits
    char* ptr = reinterpret_cast<char*>(out.data());
    int bit = 0;

    *ptr = static_cast<char>(8 - bit_count % 8);
    if (*ptr == 8) {
      *ptr = 0;
    }
    ptr++;

    for (uint32_t i = 0; i < length; i++) {
      DoWriteBits(&ptr, &bit, nodes_[static_cast<uint8_t>(data[i])].val,
                  nodes_[static_cast<uint8_t>(data[i])].bits);
    }
    // make sure we're either at the end of our allotted buffer or we're one
    // from the end and the bitcount takes care of the rest
    assert(ptr - reinterpret_cast<char*>(out.data()) == length_out
           || (ptr - reinterpret_cast<char*>(out.data()) == length_out - 1
               && bit_count != 0));
    assert(bit == bit_count % 8);

    // mark it as compressed
    out[0] |= (0x01 << 7);
    return out;
  }
#else

#if HUFFMAN_TRAINING_MODE
  train(data, length);
#endif

  data_out = data;
  length_out = length;
#endif
}

// hmmm - I saw a crash logged in this function; need to make sure this is
// bulletproof since untrusted data is coming through here..
auto Huffman::decompress(const std::vector<uint8_t>& src)
    -> std::vector<uint8_t> {
#if BA_HUFFMAN_NET_COMPRESSION

  auto length = static_cast<uint32_t>(src.size());
  BA_PRECONDITION(length > 0);

  const char* data = (const char*)src.data();

  auto remainder = static_cast<uint8_t>(*data & 0x0F);
  bool compressed = *data >> 7;

  if (compressed) {
    std::vector<uint8_t> out;
    out.reserve(src.size() * 2);  // hopefully minimize reallocations..

    uint32_t bit_length = ((length - 1) * 8);
    if (remainder > bit_length) throw Exception("invalid huffman data");
    bit_length -= remainder;
    uint32_t bit = 0;
    const char* ptr = data + 1;

    // navigate bit by bit through our nodes to build values from binary codes
    while (bit < bit_length) {
      bool bitval = static_cast<bool>((ptr[bit / 8] >> (bit % 8)) & 0x01);
      bit++;

      // 1 in first bit denotes huffman compressed
      if (bitval) {
        int val;
        int n = 510;
        BA_PRECONDITION(nodes_[n].parent == 0);
        while (true) {
          BA_PRECONDITION(n <= 510);

          bitval = static_cast<bool>((ptr[bit / 8] >> (bit % 8)) & 0x01);

          // 1 for right, 0 for left
          if (bitval == 0) {
            if (nodes_[n].left_child == -1) {
              val = n;
              break;
            } else {
              n = nodes_[n].left_child;
              bit++;
            }
          } else {
            if (nodes_[n].right_child == -1) {
              val = n;
              break;
            } else {
              n = nodes_[n].right_child;
              bit++;
            }
          }
          // ERICF FIX - if both new children are dead-ends, stop reading
          // bits; otherwise we might read past the end of the buffer.
          // (I assume the original code didn't have child nodes with dual -1s
          // so this case probably never came up)
          if (nodes_[n].left_child == -1 && nodes_[n].right_child == -1) {
            val = n;
            break;
          }

          if (bit > bit_length) {
            throw Exception("huffman decompress got bit > bitlength");
          }
        }
        out.push_back(static_cast<unsigned char&&>(val));
      } else {
        // just read next 8 bits as value
        uint8_t val;
        if (bit % 8 == 0) {
          BA_PRECONDITION((bit / 8) < (length - 1));
          val = static_cast<uint8_t>(ptr[bit / 8]);
        } else {
          BA_PRECONDITION((bit / 8 + 1) < (length - 1));
          val = (static_cast<uint8_t>(ptr[bit / 8]) >> bit % 8)
                | (static_cast<uint8_t>(ptr[bit / 8 + 1]) << (8 - bit % 8));
        }
        out.push_back(val);
        bit += 8;
        if (bit > bit_length) {
          throw Exception("huffman decompress got bit > bitlength b");
        }
      }
    }
    BA_PRECONDITION(bit == bit_length);
    return out;
  } else {
    // uncompressed - just provide it as is
    return src;
  }

#else
  data_out = data;
  length_out = length;
#endif
}

// old janky version..
#if 0
void Huffman::compress(const char* data, uint32_t length, const char*& data_out,
                       uint32_t& length_out) {
  if (length > kMaxPacketSize) {
    throw Exception("packet too large for huffman compressor: "
                        + std::to_string(length) + " (packet "
                        + std::to_string(static_cast<int>(data[0])) + ")");
  }

#if BA_HUFFMAN_NET_COMPRESSION

  // all our packets have a type bit at the beginning
  // we hijack the highest bit to denote whether we're sending
  // a compressed or uncompressed packet (1 for compressed, 0 for uncompressed)
  BA_PRECONDITION(data[0] >> 7 == 0);

  // length_out = length;
  //  if (buffer.size() < length_out)
  //      buffer.resize(length_out);
  // memcpy(buffer.data,data,length_out);

  // see how many bits we'll need
  uint32_t bit_count = 0;
  for (uint32_t i = 0; i < length; i++) {
    bit_count += nodes[static_cast<uint8_t>(data[i])].bits;
  }

  // round up to next byte and add our one-byte header
  length_out = bit_count / 8 + 1;
  if (bit_count % 8) length_out++;
  bit_count %= 8;

  // if compressed is bigger than uncompressed, go with uncompressed -
  // just return the data they provided

  // lets always do huffman in debug builds; make sure we aren't making
  // any incorrect assumptions about
  // where stuff is compressed vs uncompressed.
#if BA_DEBUG_BUILD
  bool force = false;
#else
  bool force = false;
#endif

  if ((length_out >= length) && !force) {
    // throw Exception();
    data_out = data;
    length_out = length;
  } else {
    if (buffer.size() < length_out) {
      buffer.resize(length_out);
    }

    // first byte gives our number of empty trailing bits
    memset(buffer.data, 0, buffer.size());
    char* ptr = buffer.data;
    int bit = 0;

    *ptr = (8 - bit_count % 8);
    if (*ptr == 8) *ptr = 0;
    ptr++;

    for (uint32_t i = 0; i < length; i++) {
      DoWriteBits(ptr, bit, nodes[static_cast<uint8_t>(data[i])].val,
                  nodes[static_cast<uint8_t>(data[i])].bits);
    }

    // make sure we're either at the end of our alloted buffer or we're one
    // from the end and the bitcount takes care of the rest
    assert(ptr - buffer.data == length_out
                    || (ptr - buffer.data == length_out - 1 && bit_count != 0));
    assert(bit == bit_count % 8);
    //  for (int i = 0; i < length_out;i++)
    //      buffer.data[i]--;

    data_out = buffer.data;

    // mark it as compressed
    buffer.data[0] |= (0x01 << 7);
  }
#else

#if HUFFMAN_TRAINING_MODE
  train(data, length);
#endif

  data_out = data;
  length_out = length;
#endif
}
#endif

#if 0
void Huffman::decompress(const char* data, uint32_t length,
                         const char*& data_out, uint32_t& length_out) {
#if BA_HUFFMAN_NET_COMPRESSION

  uint8_t remainder = *data & 0x0F;
  bool compressed = *data >> 7;

  if (compressed) {
    uint32_t bit_length = ((length - 1) * 8) - remainder;

    uint32_t bit = 0;
    const char* ptr = data + 1;
    uint32_t bytes = 0;

    // navigate bit by bit through our nodes to build values from binary codes
    while (bit < bit_length) {
      bool bitval = (ptr[bit / 8] >> (bit % 8)) & 0x01;
      bit++;

      // 1 in first bit denotes huffman compressed
      if (bitval) {
        int val;
        int n = 510;
        assert(nodes[n].parent == 0);
        while (true) {
          bitval = (ptr[bit / 8] >> (bit % 8)) & 0x01;

          // 1 for right, 0 for left
          if (bitval == 0) {
            if (nodes[n].left_child == -1) {
              val = n;
              break;
            } else {
              n = nodes[n].left_child;
              bit++;
            }
          } else {
            if (nodes[n].right_child == -1) {
              val = n;
              break;
            } else {
              n = nodes[n].right_child;
              bit++;
            }
          }
        }
        buffer.data[bytes] = val;
        bytes++;
      } else {
        // just read next 8 bits as value
        //          unsigned int val = (((ptr[(bit+0)/8] >> ((bit+0)%8)) & 0x01)
        //          << 0)
        //              | (((ptr[(bit+1)/8] >> ((bit+1)%8)) & 0x01) << 1)
        //              | (((ptr[(bit+2)/8] >> ((bit+2)%8)) & 0x01) << 2)
        //              | (((ptr[(bit+3)/8] >> ((bit+3)%8)) & 0x01) << 3)
        //              | (((ptr[(bit+4)/8] >> ((bit+4)%8)) & 0x01) << 4)
        //              | (((ptr[(bit+5)/8] >> ((bit+5)%8)) & 0x01) << 5)
        //              | (((ptr[(bit+6)/8] >> ((bit+6)%8)) & 0x01) << 6)
        //              | (((ptr[(bit+7)/8] >> ((bit+7)%8)) & 0x01) << 7);

        uint8_t val;
        if (bit % 8 == 0)
          val = static_cast<uint8_t>(ptr[bit / 8]);
        else
          val = (static_cast<uint8_t>(ptr[bit / 8]) >> bit % 8)
                | (static_cast<uint8_t>(ptr[bit / 8 + 1]) << (8 - bit % 8));
        //          uint8_t val2 = (((ptr[(bit+0)/8] >> ((bit+0)%8)) & 0x01) <<
        //          0)
        //              | (((ptr[(bit+1)/8] >> ((bit+1)%8)) & 0x01) << 1)
        //              | (((ptr[(bit+2)/8] >> ((bit+2)%8)) & 0x01) << 2)
        //              | (((ptr[(bit+3)/8] >> ((bit+3)%8)) & 0x01) << 3)
        //              | (((ptr[(bit+4)/8] >> ((bit+4)%8)) & 0x01) << 4)
        //              | (((ptr[(bit+5)/8] >> ((bit+5)%8)) & 0x01) << 5)
        //              | (((ptr[(bit+6)/8] >> ((bit+6)%8)) & 0x01) << 6)
        //              | (((ptr[(bit+7)/8] >> ((bit+7)%8)) & 0x01) << 7);
        //          assert(val2 == val);
        buffer.data[bytes] = val;
        bytes++;
        bit += 8;
        // throw Exception();
      }
    }
    assert(bit == bit_length);

    // fixme??
    if (bytes > kMaxPacketSize) {
      Log("HUFFMAN DECOMPRESSING TO TOO LARGE: " + std::to_string(bytes));
    }
    assert(bytes <= kMaxPacketSize);

    // throw Exception();

    // length_out = length;
    //  if (buffer.size() < length_out)
    //      buffer.resize(length_out);
    // memcpy(buffer.data,data,length_out);
    // data_out = buffer.data;
    //  for (int i = 0; i < length_out;i++)
    //      buffer.data[i]++;
    data_out = buffer.data;
    length_out = bytes;

  } else {
    // uncompressed - just provide it as is
    data_out = data;
    length_out = length;
  }
#else
  data_out = data;
  length_out = length;
#endif
}
#endif  // 0

#if HUFFMAN_TRAINING_MODE
void Huffman::train(const char* buffer, int len) {
  if (built) {
    test_bytes += len;
    for (int i = 0; i < len; i++) {
      test_bits_compressed += nodes[static_cast<uint8_t>(buffer[i])].bits;
    }
    static int poo = 0;
    poo++;
    if (poo > 100) {
      poo = 0;
      test_bytes = 0;
      test_bits_compressed = 0;
    }
    return;
  }
  total_length += len;
  while (len > 0) {
    nodes[static_cast<uint8_t>(*buffer)].frequency++;
    total_count++;
    buffer++;
    len--;
  }
  if (total_length > kTrainingLength) {
    Log("HUFFMAN TRAINING COMPLETE:");

    build();

    // spit the C array to stdout for insertion into our code
    string s = "{";
    for (int i = 0; i < 256; i++) {
      s += std::to_string(nodes[i].frequency);
      if (i < 255) s += ",";
    }
    s += "}";
    Log("FINAL: " + s);
  }
}
#endif  // HUFFMAN_TRAINING_MODE

void Huffman::build() {
  assert(!built);

  // if we're not in training mode, use our hard-coded values
#if 1
  for (int i = 0; i < 256; i++) {
    nodes_[i].frequency = g_freqs[i];
  }
#else
  // go through and set all but the top 15 or so to zero
  // this is because all smaller values will be provided in full binary
  // form and thus don't need to be influencing the graph
  for (int i = 0; i < 256; i++) {
    int bigger = 0;
    for (int j = 0; j < 256; j++) {
      if (nodes[j].frequency > nodes[i].frequency) {
        bigger++;
        if (bigger > 15) {
          nodes[i].frequency = 0;
          break;
        }
      }
    }
  }
#endif

  // first 256 nodes are leaves
  int node_count = 256;

  // now loop through existing nodes finding the two smallest values without
  // parents and creating a new parent node for them with their sum as its
  // frequency value once there's only 1 node without a parent we're done
  // (that's the root node)
  int smallest1;
  int smallest2;
  while (node_count < 511) {
    int i = 0;

    // find first two non-parented nodes
    while (nodes_[i].parent != 0) i++;
    smallest1 = i;
    i++;
    while (nodes_[i].parent != 0) i++;
    smallest2 = i;
    i++;
    while (i < node_count) {
      if (nodes_[i].parent == 0) {
        // compare each node to the larger of the two existing to try and knock
        // it off
        if (nodes_[smallest1].frequency > nodes_[smallest2].frequency) {
          if (nodes_[i].frequency < nodes_[smallest1].frequency) smallest1 = i;
        } else {
          if (nodes_[i].frequency < nodes_[smallest2].frequency) smallest2 = i;
        }
      }
      i++;
    }
    nodes_[node_count].frequency =
        nodes_[smallest1].frequency + nodes_[smallest2].frequency;
    nodes_[smallest1].parent = static_cast<uint8_t>(node_count - 255);
    nodes_[smallest2].parent = static_cast<uint8_t>(node_count - 255);
    nodes_[node_count].right_child = static_cast<int16_t>(smallest1);
    nodes_[node_count].left_child = static_cast<int16_t>(smallest2);

    node_count++;
  }

  assert(nodes_[509].parent != 0);
  assert(nodes_[510].parent == 0);

  // now store binary values for each base value (0-255)
  for (int i = 0; i < 256; i++) {
    // uint32_t val = 0;
    nodes_[i].val = 0;
    nodes_[i].bits = 0;
    int index = i;
    while (nodes_[index].parent != 0) {
      // 0 if we're left child, 1 if we're right
      if (nodes_[nodes_[index].parent + 255].right_child == index) {
        nodes_[i].val = static_cast<uint16_t>(nodes_[i].val << 1 | 0x01);
      } else {
        assert(nodes_[nodes_[index].parent + 255].left_child == index);
        nodes_[i].val = nodes_[i].val << 1;
      }
      nodes_[i].bits++;

      index = nodes_[index].parent + 255;
    }
    // we're slightly different than normal huffman in that
    // our first bit denotes whether the following values are the huffman bits
    // or the full 8 bit value.
    if (nodes_[i].bits >= 8) {
      nodes_[i].bits = 8;
      // nodes[i].val = nodes[i].val << 1;
      nodes_[i].val = static_cast<uint16_t>(i << 1);
    } else {
      nodes_[i].val = static_cast<uint16_t>(
          nodes_[i].val << 1
          | 0x01);  // 1 in first bit denotes huffman compressed
    }
    // nodes[i].val = 0;
    nodes_[i].bits += 1;
  }

  built = true;
}

#pragma clang diagnostic pop

}  // namespace ballistica
