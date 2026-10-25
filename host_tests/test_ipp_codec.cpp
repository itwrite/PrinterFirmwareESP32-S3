// test_ipp_codec.cpp - IPP binary header, attributes and document boundary tests.
#include "assert.h"

#include <cstdint>
#include <cstring>

#include "airprint_core/ipp_attributes.h"
#include "airprint_core/ipp_codec.h"

using namespace ijpd::airprint;

namespace {

bool is_attribute(const IppReadResult& result, const char* name,
                 IppValueTag tag, const char* value) {
    if (result.error != IppError::Ok || result.kind != IppReadKind::Attribute ||
        !attribute_name_equals(result.attribute, name) ||
        result.attribute.value_tag != tag) {
        return false;
    }
    const size_t length = std::strlen(value);
    return result.attribute.value_length == length &&
           std::memcmp(result.attribute.value, value, length) == 0;
}

}  // namespace

int main() {
    uint8_t bytes[512] = {};
    IppWriter writer(bytes, sizeof(bytes));
    CHECK_TRUE(writer.begin_request({2, 0}, IppOperation::PrintJob, 0x01020304) ==
               IppError::Ok);
    CHECK_TRUE(writer.begin_group(IppGroupTag::OperationAttributes) == IppError::Ok);
    CHECK_TRUE(writer.add_cstr(IppValueTag::Charset, "attributes-charset", "utf-8") ==
               IppError::Ok);
    CHECK_TRUE(writer.add_cstr(IppValueTag::NaturalLanguage,
                               "attributes-natural-language", "en") == IppError::Ok);
    CHECK_TRUE(writer.add_cstr(IppValueTag::Uri, "printer-uri",
                               "ipp://printer.local/ipp/print") == IppError::Ok);
    CHECK_TRUE(writer.add_cstr(IppValueTag::MimeMediaType, "document-format",
                               "image/urf") == IppError::Ok);
    CHECK_TRUE(writer.end_attributes() == IppError::Ok);
    const uint8_t document[] = {0x00, 0x01, 0x7F, 0xFF};
    CHECK_TRUE(writer.append_document(document, sizeof(document)) == IppError::Ok);

    CHECK_EQ(writer.data()[0], 0x02);
    CHECK_EQ(writer.data()[1], 0x00);
    CHECK_EQ_HEX(static_cast<uint16_t>((writer.data()[2] << 8) | writer.data()[3]),
                 static_cast<uint16_t>(IppOperation::PrintJob));
    CHECK_EQ_HEX(static_cast<uint32_t>((writer.data()[4] << 24) |
                                       (writer.data()[5] << 16) |
                                       (writer.data()[6] << 8) | writer.data()[7]),
                 0x01020304u);

    IppReader reader;
    CHECK_TRUE(reader.begin(writer.data(), writer.length()) == IppError::Ok);
    CHECK_EQ(reader.header().version.major, 2);
    CHECK_EQ(reader.header().version.minor, 0);
    CHECK_EQ_HEX(reader.header().code, 0x0002);
    CHECK_EQ_HEX(reader.header().request_id, 0x01020304u);

    IppReadResult result = reader.next();
    CHECK_TRUE(result.error == IppError::Ok && result.kind == IppReadKind::GroupStart);
    CHECK_TRUE(result.group == IppGroupTag::OperationAttributes);
    CHECK_TRUE(is_attribute(reader.next(), "attributes-charset", IppValueTag::Charset, "utf-8"));
    CHECK_TRUE(is_attribute(reader.next(), "attributes-natural-language",
                            IppValueTag::NaturalLanguage, "en"));
    CHECK_TRUE(is_attribute(reader.next(), "printer-uri", IppValueTag::Uri,
                            "ipp://printer.local/ipp/print"));
    CHECK_TRUE(is_attribute(reader.next(), "document-format", IppValueTag::MimeMediaType,
                            "image/urf"));
    result = reader.next();
    CHECK_TRUE(result.error == IppError::Ok &&
               result.kind == IppReadKind::EndOfAttributes);
    CHECK_EQ(reader.document_length(), sizeof(document));
    CHECK_EQ_MEM(reader.document_data(), document, sizeof(document));
    CHECK_TRUE(reader.next().kind == IppReadKind::EndOfAttributes);

    uint8_t continuation_bytes[256] = {};
    IppWriter continuation_writer(continuation_bytes, sizeof(continuation_bytes));
    CHECK_TRUE(continuation_writer.begin_response({2, 0}, IppStatus::SuccessfulOk, 7) ==
               IppError::Ok);
    CHECK_TRUE(continuation_writer.begin_group(IppGroupTag::PrinterAttributes) ==
               IppError::Ok);
    CHECK_TRUE(continuation_writer.add_cstr(IppValueTag::Keyword, "media-supported",
                                            "iso_a4_210x297mm") == IppError::Ok);
    const char continuation_value[] = "na_letter_8.5x11in";
    CHECK_TRUE(continuation_writer.add_continuation(
                   IppValueTag::Keyword,
                   reinterpret_cast<const uint8_t*>(continuation_value),
                   sizeof(continuation_value) - 1) ==
               IppError::Ok);
    CHECK_TRUE(continuation_writer.end_attributes() == IppError::Ok);
    IppReader continuation_reader;
    CHECK_TRUE(continuation_reader.begin(continuation_writer.data(),
                                         continuation_writer.length()) == IppError::Ok);
    CHECK_TRUE(continuation_reader.next().kind == IppReadKind::GroupStart);
    IppReadResult first = continuation_reader.next();
    IppReadResult second = continuation_reader.next();
    CHECK_TRUE(first.error == IppError::Ok && !first.attribute.continuation);
    CHECK_TRUE(second.error == IppError::Ok && second.attribute.continuation);
    CHECK_EQ(first.attribute.name_length, second.attribute.name_length);
    CHECK_EQ_MEM(first.attribute.name, second.attribute.name, first.attribute.name_length);

    uint8_t collection_bytes[256] = {};
    IppWriter collection_writer(collection_bytes, sizeof(collection_bytes));
    CHECK_TRUE(collection_writer.begin_response({2, 0}, IppStatus::SuccessfulOk, 8) ==
               IppError::Ok);
    CHECK_TRUE(collection_writer.begin_group(IppGroupTag::PrinterAttributes) ==
               IppError::Ok);
    CHECK_TRUE(collection_writer.begin_collection("media-col-default") == IppError::Ok);
    CHECK_TRUE(collection_writer.add_collection_member_name("media-keyword") ==
               IppError::Ok);
    CHECK_TRUE(collection_writer.add_continuation(
                   IppValueTag::Keyword,
                   reinterpret_cast<const uint8_t*>("iso_a4_210x297mm"),
                   std::strlen("iso_a4_210x297mm")) == IppError::Ok);
    CHECK_TRUE(collection_writer.end_collection() == IppError::Ok);
    CHECK_TRUE(collection_writer.end_attributes() == IppError::Ok);
    IppReader collection_reader;
    CHECK_TRUE(collection_reader.begin(collection_writer.data(), collection_writer.length()) ==
               IppError::Ok);
    CHECK_TRUE(collection_reader.next().kind == IppReadKind::GroupStart);
    CHECK_TRUE(collection_reader.next().attribute.value_tag == IppValueTag::BegCollection);
    CHECK_TRUE(collection_reader.next().attribute.value_tag == IppValueTag::MemberAttrName);
    CHECK_TRUE(collection_reader.next().attribute.value_tag == IppValueTag::Keyword);
    CHECK_TRUE(collection_reader.next().attribute.value_tag == IppValueTag::EndCollection);

    // Nested media-col/media-size collection framing must remain consumable.
    uint8_t nested_bytes[512] = {};
    IppWriter nested_writer(nested_bytes, sizeof(nested_bytes));
    CHECK_TRUE(nested_writer.begin_request({2, 0}, IppOperation::PrintJob, 9) ==
               IppError::Ok);
    CHECK_TRUE(nested_writer.begin_group(IppGroupTag::OperationAttributes) == IppError::Ok);
    CHECK_TRUE(nested_writer.begin_group(IppGroupTag::JobAttributes) == IppError::Ok);
    CHECK_TRUE(nested_writer.begin_collection("media-col") == IppError::Ok);
    CHECK_TRUE(nested_writer.add_collection_member_name("media-size") == IppError::Ok);
    CHECK_TRUE(nested_writer.add_continuation(IppValueTag::BegCollection, nullptr, 0) ==
               IppError::Ok);
    CHECK_TRUE(nested_writer.add_collection_member_name("x-dimension") == IppError::Ok);
    const uint8_t x_dimension[] = {0, 0, 0x52, 0x08};
    CHECK_TRUE(nested_writer.add_continuation(IppValueTag::Integer, x_dimension,
                                               sizeof(x_dimension)) == IppError::Ok);
    CHECK_TRUE(nested_writer.add_collection_member_name("y-dimension") == IppError::Ok);
    const uint8_t y_dimension[] = {0, 0, 0x73, 0x14};
    CHECK_TRUE(nested_writer.add_continuation(IppValueTag::Integer, y_dimension,
                                               sizeof(y_dimension)) == IppError::Ok);
    CHECK_TRUE(nested_writer.end_collection() == IppError::Ok);
    CHECK_TRUE(nested_writer.end_collection() == IppError::Ok);
    CHECK_TRUE(nested_writer.end_attributes() == IppError::Ok);
    IppReader nested_reader;
    CHECK_TRUE(nested_reader.begin(nested_writer.data(), nested_writer.length()) == IppError::Ok);
    size_t nested_items = 0;
    while (true) {
        const IppReadResult item = nested_reader.next();
        CHECK_TRUE(item.error == IppError::Ok);
        if (item.kind == IppReadKind::EndOfAttributes) break;
        ++nested_items;
    }
    CHECK_TRUE(nested_items >= 9u);

    CHECK_TRUE(find_attribute_spec("document-format", 15) != nullptr);
    CHECK_TRUE(find_attribute_spec("not-a-real-attribute", 21) == nullptr);

    uint8_t tiny[12] = {};
    IppWriter tiny_writer(tiny, sizeof(tiny));
    CHECK_TRUE(tiny_writer.begin_request({2, 0}, IppOperation::PrintJob, 1) ==
               IppError::Ok);
    CHECK_TRUE(tiny_writer.begin_group(IppGroupTag::OperationAttributes) == IppError::Ok);
    CHECK_TRUE(tiny_writer.add_cstr(IppValueTag::Charset, "attributes-charset", "utf-8") ==
               IppError::OutputTooSmall);

    CHECK_TRUE(reader.begin(bytes, 7) == IppError::Truncated);
    uint8_t malformed[] = {2, 0, 0, 2, 0, 0, 0, 1,
                           static_cast<uint8_t>(IppGroupTag::OperationAttributes),
                           static_cast<uint8_t>(IppValueTag::Charset), 0, 1, 'x', 0, 8};
    CHECK_TRUE(reader.begin(malformed, sizeof(malformed)) == IppError::Ok);
    CHECK_TRUE(reader.next().kind == IppReadKind::GroupStart);
    CHECK_TRUE(reader.next().error == IppError::Truncated);

    return ::ijpd::test::summary("test_ipp_codec");
}
