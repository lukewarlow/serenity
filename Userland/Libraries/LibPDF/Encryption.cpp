/*
 * Copyright (c) 2022, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <AK/Debug.h>
#include <AK/Random.h>
#include <LibCrypto/Cipher/AES.h>
#include <LibCrypto/Hash/MD5.h>
#include <LibPDF/CommonNames.h>
#include <LibPDF/Document.h>
#include <LibPDF/Encryption.h>

namespace PDF {

static constexpr Array<u8, 32> standard_encryption_key_padding_bytes = {
    0x28,
    0xBF,
    0x4E,
    0x5E,
    0x4E,
    0x75,
    0x8A,
    0x41,
    0x64,
    0x00,
    0x4E,
    0x56,
    0xFF,
    0xFA,
    0x01,
    0x08,
    0x2E,
    0x2E,
    0x00,
    0xB6,
    0xD0,
    0x68,
    0x3E,
    0x80,
    0x2F,
    0x0C,
    0xA9,
    0xFE,
    0x64,
    0x53,
    0x69,
    0x7A,
};

PDFErrorOr<NonnullRefPtr<SecurityHandler>> SecurityHandler::create(Document* document, NonnullRefPtr<DictObject> encryption_dict)
{
    auto filter = TRY(encryption_dict->get_name(document, CommonNames::Filter))->name();
    if (filter == "Standard")
        return TRY(StandardSecurityHandler::create(document, encryption_dict));

    dbgln("Unrecognized security handler filter: {}", filter);
    TODO();
}

struct CryptFilter {
    CryptFilterMethod method { CryptFilterMethod::None };
    int length_in_bits { 0 };
};

static PDFErrorOr<CryptFilter> parse_v4_or_newer_crypt(Document* document, NonnullRefPtr<DictObject> encryption_dict, DeprecatedString filter)
{
    // See 3.5 Encryption, Table 3.18 "Entries common to all encryption dictionaries" for StmF and StrF,
    // and 3.5.4 Crypt Filters in the 1.7 spec, in particular Table 3.22 "Entries common to all crypt filter dictionaries".

    if (filter == "Identity")
        return CryptFilter {};

    // "Every crypt filter used in the document must have an entry in this dictionary"
    if (!encryption_dict->contains(CommonNames::CF))
        return Error(Error::Type::Parse, "Missing CF key in encryption dict for v4");

    auto crypt_filter_dicts = TRY(encryption_dict->get_dict(document, CommonNames::CF));
    if (!crypt_filter_dicts->contains(filter))
        return Error(Error::Type::Parse, "Missing key in CF dict for v4");

    auto crypt_filter_dict = TRY(crypt_filter_dicts->get_dict(document, filter));

    // "Default value: None"
    if (!crypt_filter_dict->contains(CommonNames::CFM))
        return CryptFilter {};
    auto crypt_filter_method = TRY(crypt_filter_dict->get_name(document, CommonNames::CFM))->name();
    if (crypt_filter_method == "None")
        return CryptFilter {};

    // Table 3.22 in the 1.7 spec says this is optional but doesn't give a default value.
    // But the 2.0 spec (ISO 32000 2020) says it's required.
    // The 2.0 spec also says "The standard security handler expresses the Length entry in bytes" (!).
    if (!crypt_filter_dict->contains(CommonNames::Length))
        return Error(Error::Type::Parse, "crypt filter /Length missing");
    auto length_in_bits = crypt_filter_dict->get_value(CommonNames::Length).get<int>() * 8;

    // NOTE: /CFM's /AuthEvent should be ignored for /StmF, /StrF.

    if (crypt_filter_method == "V2")
        return CryptFilter { CryptFilterMethod::V2, length_in_bits };

    if (crypt_filter_method == "AESV2") {
        // "the AES algorithm in Cipher Block Chaining (CBC) mode with a 16-byte block size [...] The key size (Length) shall be 128 bits."
        if (length_in_bits != 128)
            return Error(Error::Type::Parse, "Unexpected bit size for AESV2");
        return CryptFilter { CryptFilterMethod::AESV2, length_in_bits };
    }

    if (crypt_filter_method == "AESV3") {
        // "the AES-256 algorithm in Cipher Block Chaining (CBC) with padding mode with a 16-byte block size [...] The key size (Length) shall be 256 bits."
        if (length_in_bits != 256)
            return Error(Error::Type::Parse, "Unexpected bit size for AESV3");
        return CryptFilter { CryptFilterMethod::AESV3, length_in_bits };
    }

    return Error(Error::Type::Parse, "Unknown crypt filter method");
}

PDFErrorOr<NonnullRefPtr<StandardSecurityHandler>> StandardSecurityHandler::create(Document* document, NonnullRefPtr<DictObject> encryption_dict)
{
    auto revision = encryption_dict->get_value(CommonNames::R).get<int>();
    auto o = TRY(encryption_dict->get_string(document, CommonNames::O))->string();
    auto u = TRY(encryption_dict->get_string(document, CommonNames::U))->string();
    auto p = encryption_dict->get_value(CommonNames::P).get<int>();

    // V, number: [...] 1 "Algorithm 1 Encryption of data using the RC4 or AES algorithms" in 7.6.2,
    // "General Encryption Algorithm," with an encryption key length of 40 bits, see below [...]
    // Length, integer: (Optional; PDF 1.4; only if V is 2 or 3) The length of the encryption key, in bits.
    // The value shall be a multiple of 8, in the range 40 to 128. Default value: 40.
    auto v = encryption_dict->get_value(CommonNames::V).get<int>();

    auto method = CryptFilterMethod::V2;
    size_t length_in_bits = 40;

    if (v >= 4) {
        // "Default value: Identity"
        DeprecatedString stream_filter = "Identity";
        if (encryption_dict->contains(CommonNames::StmF))
            stream_filter = TRY(encryption_dict->get_name(document, CommonNames::StmF))->name();

        DeprecatedString string_filter = "Identity";
        if (encryption_dict->contains(CommonNames::StrF))
            string_filter = TRY(encryption_dict->get_name(document, CommonNames::StrF))->name();

        if (stream_filter != string_filter)
            return Error(Error::Type::Parse, "Can't handle StmF and StrF being different");

        auto crypt_filter = TRY(parse_v4_or_newer_crypt(document, encryption_dict, stream_filter));
        method = crypt_filter.method;
        length_in_bits = crypt_filter.length_in_bits;
    } else if (encryption_dict->contains(CommonNames::Length))
        length_in_bits = encryption_dict->get_value(CommonNames::Length).get<int>();
    else if (v != 1)
        return Error(Error::Type::Parse, "Can't determine length of encryption key");

    auto length = length_in_bits / 8;

    dbgln_if(PDF_DEBUG, "encryption v{}, method {}, length {}", v, (int)method, length);

    bool encrypt_metadata = true;
    if (encryption_dict->contains(CommonNames::EncryptMetadata))
        encryption_dict->get_value(CommonNames::EncryptMetadata).get<bool>();

    return adopt_ref(*new StandardSecurityHandler(document, revision, o, u, p, encrypt_metadata, length, method));
}

StandardSecurityHandler::StandardSecurityHandler(Document* document, size_t revision, DeprecatedString const& o_entry, DeprecatedString const& u_entry, u32 flags, bool encrypt_metadata, size_t length, CryptFilterMethod method)
    : m_document(document)
    , m_revision(revision)
    , m_o_entry(o_entry)
    , m_u_entry(u_entry)
    , m_flags(flags)
    , m_encrypt_metadata(encrypt_metadata)
    , m_length(length)
    , m_method(method)
{
}

ByteBuffer StandardSecurityHandler::compute_user_password_value_r2(ByteBuffer password_string)
{
    // Algorithm 4: Computing the encryption dictionary's U (user password)
    //              value (Security handlers of revision 2)

    // a) Create an encryption key based on the user password string, as
    //    described in [Algorithm 2]
    auto encryption_key = compute_encryption_key_r2_to_r5(password_string);

    // b) Encrypt the 32-byte padding string shown in step (a) of [Algorithm 2],
    //    using an RC4 encryption function with the encryption key from the
    //    preceding step.
    RC4 rc4(encryption_key);
    auto output = rc4.encrypt(standard_encryption_key_padding_bytes);

    // c) Store the result of step (b) as the value of the U entry in the
    //    encryption dictionary.
    return output;
}

ByteBuffer StandardSecurityHandler::compute_user_password_value_r3_to_r5(ByteBuffer password_string)
{
    // Algorithm 5: Computing the encryption dictionary's U (user password)
    //              value (Security handlers of revision 3 or greater)

    // a) Create an encryption key based on the user password string, as
    //    described in [Algorithm 2]
    auto encryption_key = compute_encryption_key_r2_to_r5(password_string);

    // b) Initialize the MD5 hash function and pass the 32-byte padding string
    //    shown in step (a) of [Algorithm 2] as input to this function
    Crypto::Hash::MD5 md5;
    md5.update(standard_encryption_key_padding_bytes);

    // e) Pass the first element of the file's file identifier array to the MD5
    //    hash function.
    auto id_array = MUST(m_document->trailer()->get_array(m_document, CommonNames::ID));
    auto first_element_string = MUST(id_array->get_string_at(m_document, 0))->string();
    md5.update(first_element_string);

    // d) Encrypt the 16-byte result of the hash, using an RC4 encryption function
    //    with the encryption key from step (a).
    RC4 rc4(encryption_key);
    auto out = md5.peek();
    auto buffer = rc4.encrypt(out.bytes());

    // e) Do the following 19 times:
    //
    //    Take the output from the previous invocation of the RC4 function and pass
    //    it as input to a new invocation of the function; use an encryption key generated
    //    by taking each byte of the original encryption key obtained in step (a) and
    //    performing an XOR operation between the that byte and the single-byte value of
    //    the iteration counter (from 1 to 19).
    auto new_encryption_key = MUST(ByteBuffer::create_uninitialized(encryption_key.size()));
    for (size_t i = 1; i <= 19; i++) {
        for (size_t j = 0; j < encryption_key.size(); j++)
            new_encryption_key[j] = encryption_key[j] ^ i;

        RC4 new_rc4(new_encryption_key);
        buffer = new_rc4.encrypt(buffer);
    }

    // f) Append 16 bytes of the arbitrary padding to the output from the final invocation
    //    of the RC4 function and store the 32-byte result as the value of the U entry in
    //    the encryption dictionary.
    VERIFY(buffer.size() == 16);
    for (size_t i = 0; i < 16; i++)
        buffer.append(0xab);

    return buffer;
}

bool StandardSecurityHandler::authenticate_user_password_r2_to_r5(StringView password_string)
{
    // Algorithm 6: Authenticating the user password

    // a) Perform all but the last step of [Algorithm 4] or [Algorithm 5] using the
    //    supplied password string.
    ByteBuffer password_buffer = MUST(ByteBuffer::copy(password_string.bytes()));
    if (m_revision == 2) {
        password_buffer = compute_user_password_value_r2(password_buffer);
    } else {
        password_buffer = compute_user_password_value_r3_to_r5(password_buffer);
    }

    // b) If the result of step (a) is equal to the value of the encryption
    //    dictionary's "U" entry (comparing the first 16 bytes in the case of security
    //    handlers of revision 3 or greater), the password supplied is the correct user
    //    password.
    auto u_bytes = m_u_entry.bytes();
    if (m_revision >= 3)
        return u_bytes.slice(0, 16) == password_buffer.bytes().slice(0, 16);
    return u_bytes == password_buffer.bytes();
}

bool StandardSecurityHandler::authenticate_user_password_r6_and_later(StringView)
{
    // ISO 32000 (PDF 2.0), 7.6.4.4.10 Algorithm 11: Authenticating the user password (Security handlers of
    // revision 6)

    // a) Test the password against the user key by computing the 32-byte hash using 7.6.4.3.4, "Algorithm 2.B:
    //    Computing a hash (revision 6 or later)" with an input string consisting of the UTF-8 password
    //    concatenated with the 8 bytes of User Validation Salt (see 7.6.4.4.7, "Algorithm 8: Computing the
    //    encryption dictionary's U (user password) and UE (user encryption) values (Security handlers of
    //    revision 6)"). If the 32- byte result matches the first 32 bytes of the U string, this is the user password.
    TODO();
}

bool StandardSecurityHandler::try_provide_user_password(StringView password_string)
{
    bool has_user_password;
    if (m_revision >= 6)
        has_user_password = authenticate_user_password_r6_and_later(password_string);
    else
        has_user_password = authenticate_user_password_r2_to_r5(password_string);

    if (!has_user_password)
        m_encryption_key = {};
    return has_user_password;
}

ByteBuffer StandardSecurityHandler::compute_encryption_key_r2_to_r5(ByteBuffer password_string)
{
    // This function should never be called after we have a valid encryption key.
    VERIFY(!m_encryption_key.has_value());

    // 7.6.3.3 Encryption Key Algorithm

    // Algorithm 2: Computing an encryption key

    // a) Pad or truncate the password string to exactly 32 bytes. If the password string
    //    is more than 32 bytes long, use only its first 32 bytes; if it is less than 32
    //    bytes long, pad it by appending the required number of additional bytes from the
    //    beginning of the following padding string: [omitted]

    if (password_string.size() > 32) {
        password_string.resize(32);
    } else {
        password_string.append(standard_encryption_key_padding_bytes.data(), 32 - password_string.size());
    }

    // b) Initialize the MD5 hash function and pass the result of step (a) as input to
    //    this function.
    Crypto::Hash::MD5 md5;
    md5.update(password_string);

    // c) Pass the value of the encryption dictionary's "O" entry to the MD5 hash function.
    md5.update(m_o_entry);

    // d) Convert the integer value of the P entry to a 32-bit unsigned binary number and pass
    //    these bytes to the MD5 hash function, low-order byte first.
    md5.update(reinterpret_cast<u8 const*>(&m_flags), sizeof(m_flags));

    // e) Pass the first element of the file's file identifier array to the MD5 hash function.
    auto id_array = MUST(m_document->trailer()->get_array(m_document, CommonNames::ID));
    auto first_element_string = MUST(id_array->get_string_at(m_document, 0))->string();
    md5.update(first_element_string);

    // f) (Security handlers of revision 4 or greater) if the document metadata is not being
    //    encrypted, pass 4 bytes with the value 0xffffffff to the MD5 hash function.
    if (m_revision >= 4 && !m_encrypt_metadata) {
        u32 value = 0xffffffff;
        md5.update(reinterpret_cast<u8 const*>(&value), 4);
    }

    // g) Finish the hash.
    // h) (Security handlers of revision 3 or greater) Do the following 50 times:
    //
    //    Take the output from the previous MD5 hash and pass the first n bytes
    //    of the output as input into a new MD5 hash, where n is the number of
    //    bytes of the encryption key as defined by the value of the encryption
    //    dictionary's Length entry.
    if (m_revision >= 3) {
        ByteBuffer n_bytes;

        for (u32 i = 0; i < 50; i++) {
            Crypto::Hash::MD5 new_md5;
            n_bytes.ensure_capacity(m_length);

            while (n_bytes.size() < m_length) {
                auto out = md5.peek();
                for (size_t j = 0; j < out.data_length() && n_bytes.size() < m_length; j++)
                    n_bytes.append(out.data[j]);
            }

            VERIFY(n_bytes.size() == m_length);
            new_md5.update(n_bytes);
            md5 = move(new_md5);
            n_bytes.clear();
        }
    }

    // i) Set the encryption key to the first n bytes of the output from the final MD5
    //    hash, where n shall always be 5 for security handlers of revision 2 but, for
    //    security handlers of revision 3 or greater, shall depend on the value of the
    //    encryption dictionary's Length entry.
    size_t n;
    if (m_revision == 2) {
        n = 5;
    } else if (m_revision >= 3) {
        n = m_length;
    } else {
        VERIFY_NOT_REACHED();
    }

    ByteBuffer encryption_key;
    encryption_key.ensure_capacity(n);
    while (encryption_key.size() < n) {
        auto out = md5.peek();
        for (size_t i = 0; encryption_key.size() < n && i < out.data_length(); i++)
            encryption_key.append(out.bytes()[i]);
    }

    m_encryption_key = encryption_key;

    return encryption_key;
}

ByteBuffer StandardSecurityHandler::compute_encryption_key_r6_and_later(ByteBuffer password_string)
{
    // This function should never be called after we have a valid encryption key.
    VERIFY(!m_encryption_key.has_value());

    // ISO 32000 (PDF 2.0), 7.6.4.3.3 Algorithm 2.A: Retrieving the file encryption key from an encrypted
    // document in order to decrypt it (revision 6 or later)

    // "It is necessary to treat the 48-bytes of the O and U strings in the
    //  Encrypt dictionary as made up of three sections [...]. The first 32 bytes
    //  are a hash value (explained below). The next 8 bytes are called the Validation Salt. The final 8 bytes are
    //  called the Key Salt."

    // a) The UTF-8 password string shall be generated from Unicode input by processing the input string with
    //    the SASLprep (Internet RFC 4013) profile of stringprep (Internet RFC 3454) using the Normalize and BiDi
    //    options, and then converting to a UTF-8 representation.
    // FIXME

    // b) Truncate the UTF-8 representation to 127 bytes if it is longer than 127 bytes.
    if (password_string.size() > 127)
        password_string.resize(127);

    // c) Test the password against the owner key by computing a hash using algorithm 2.B with an input string
    //    consisting of the UTF-8 password concatenated with the 8 bytes of owner Validation Salt, concatenated
    //    with the 48-byte U string. If the 32-byte result matches the first 32 bytes of the O string, this is the owner
    //    password.

    // d) Compute an intermediate owner key by computing a hash using algorithm 2.B with an input string
    //    consisting of the UTF-8 owner password concatenated with the 8 bytes of owner Key Salt, concatenated
    //    with the 48-byte U string. The 32-byte result is the key used to decrypt the 32-byte OE string using AES-
    //    256 in CBC mode with no padding and an initialization vector of zero. The 32-byte result is the file
    //    encryption key.

    // e) Compute an intermediate user key by computing a hash using algorithm 2.B with an input string
    //    consisting of the UTF-8 user password concatenated with the 8 bytes of user Key Salt. The 32-byte result
    //    is the key used to decrypt the 32-byte UE string using AES-256 in CBC mode with no padding and an
    //    initialization vector of zero. The 32-byte result is the file encryption key.

    // f) Decrypt the 16-bye Perms string using AES-256 in ECB mode with an initialization vector of zero and
    //    the file encryption key as the key. Verify that bytes 9-11 of the result are the characters "a", "d", "b". Bytes
    //    0-3 of the decrypted Perms entry, treated as a little-endian integer, are the user permissions. They shall
    //    match the value in the P key.

    TODO();
}

ByteBuffer StandardSecurityHandler::computing_a_hash_r6_and_later(ByteBuffer)
{
    // ISO 32000 (PDF 2.0), 7.6.4.3.4 Algorithm 2.B: Computing a hash (revision 6 or later)

    // Take the SHA-256 hash of the original input to the algorithm and name the resulting 32 bytes, K.
    // Perform the following steps (a)-(d) 64 times:

    // a) Make a new string, K1, consisting of 64 repetitions of the sequence: Input password, K, the 48-byte user
    //    key. The 48 byte user key is only used when checking the owner password or creating the owner key. If
    //    checking the user password or creating the user key, K1 is the concatenation of the input password and K.

    // b) Encrypt K1 with the AES-128 (CBC, no padding) algorithm, using the first 16 bytes of K as the key and
    //    the second 16 bytes of K as the initialization vector. The result of this encryption is E.

    // c) Taking the first 16 bytes of E as an unsigned big-endian integer, compute the remainder, modulo 3. If the
    //    result is 0, the next hash used is SHA-256, if the result is 1, the next hash used is SHA-384, if the result is
    //    2, the next hash used is SHA-512.

    // d) Using the hash algorithm determined in step c, take the hash of E. The result is a new value of K, which
    //    will be 32, 48, or 64 bytes in length.

    // Repeat the process (a-d) with this new value of K. Following 64 rounds (round number 0 to round
    // number 63), do the following, starting with round number 64:

    // NOTE 2 The reason for multiple rounds is to defeat the possibility of running all paths in parallel. With 64
    //        rounds (minimum) there are 3^64 paths through the algorithm.

    // e) Look at the very last byte of E. If the value of that byte (taken as an unsigned integer) is greater than the
    //    round number - 32, repeat steps (a-d) again.

    // f) Repeat from steps (a-e) until the value of the last byte is <= (round number) - 32.

    // NOTE 3 Tests indicate that the total number of rounds will most likely be between 65 and 80.

    // The first 32 bytes of the final K are the output of the algorithm.

    TODO();
}

void StandardSecurityHandler::crypt(NonnullRefPtr<Object> object, Reference reference, Crypto::Cipher::Intent direction) const
{
    VERIFY(m_encryption_key.has_value());

    if (m_method == CryptFilterMethod::None)
        return;

    if (m_method == CryptFilterMethod::AESV3) {
        // ISO 32000 (PDF 2.0), 7.6.3.3 Algorithm 1.A: Encryption of data using the AES algorithms

        // a) Use the 32-byte file encryption key for the AES-256 symmetric key algorithm, along with the string or
        //    stream data to be encrypted.
        //
        //    Use the AES algorithm in Cipher Block Chaining (CBC) mode, which requires an initialization
        //    vector. The block size parameter is set to 16 bytes, and the initialization vector is a 16-byte random
        //    number that is stored as the first 16 bytes of the encrypted stream or string.
        TODO();
    }

    // 7.6.2 General Encryption Algorithm
    // Algorithm 1: Encryption of data using the RC3 or AES algorithms

    // a) Obtain the object number and generation number from the object identifier of
    //    the string or stream to be encrypted. If the string is a direct object, use
    //    the identifier of the indirect object containing it.
    //
    // Note: This is always passed in at parse time because objects don't know their own
    //       object number.

    // b) For all strings and streams with crypt filter specifier; treating the object
    //    number as binary integers, extend the original n-byte encryption key to n + 5
    //    bytes by appending the low-order 3 bytes of the object number and the low-order
    //    2 bytes of the generation number in that order, low-order byte first. ...

    auto encryption_key = m_encryption_key.value();
    ReadonlyBytes bytes;
    Function<void(ReadonlyBytes)> assign;

    if (object->is<StreamObject>()) {
        auto stream = object->cast<StreamObject>();
        bytes = stream->bytes();

        assign = [&object](ReadonlyBytes bytes) {
            object->cast<StreamObject>()->buffer() = MUST(ByteBuffer::copy(bytes));
        };

        if (stream->dict()->contains(CommonNames::Filter)) {
            auto filter = MUST(stream->dict()->get_name(m_document, CommonNames::Filter))->name();
            if (filter == "Crypt")
                TODO();
        }
    } else if (object->is<StringObject>()) {
        auto string = object->cast<StringObject>();
        bytes = string->string().bytes();
        assign = [&object](ReadonlyBytes bytes) {
            object->cast<StringObject>()->set_string(DeprecatedString(bytes));
        };
    } else {
        VERIFY_NOT_REACHED();
    }

    auto index = reference.as_ref_index();
    auto generation = reference.as_ref_generation_index();

    encryption_key.append(index & 0xff);
    encryption_key.append((index >> 8) & 0xff);
    encryption_key.append((index >> 16) & 0xff);
    encryption_key.append(generation & 0xff);
    encryption_key.append((generation >> 8) & 0xff);

    if (m_method == CryptFilterMethod::AESV2) {
        encryption_key.append('s');
        encryption_key.append('A');
        encryption_key.append('l');
        encryption_key.append('T');
    }

    // c) Initialize the MD5 hash function and pass the result of step (b) as input to this
    //    function.
    Crypto::Hash::MD5 md5;
    md5.update(encryption_key);

    // d) Use the first (n + 5) bytes, up to a maximum of 16, of the output from the MD5
    //    hash as the key for the RC4 or AES symmetric key algorithms, along with the string
    //    or stream data to be encrypted.
    auto key = MUST(ByteBuffer::copy(md5.peek().bytes()));

    if (key.size() > min(encryption_key.size(), 16))
        key.resize(encryption_key.size());

    if (m_method == CryptFilterMethod::AESV2) {
        auto cipher = Crypto::Cipher::AESCipher::CBCMode(key, m_length * 8, direction, Crypto::Cipher::PaddingMode::CMS);

        // "The block size parameter is 16 bytes, and the initialization vector is a 16-byte random number
        //  that is stored as the first 16 bytes of the encrypted stream or string."
        static_assert(Crypto::Cipher::AESCipher::block_size() == 16);
        if (direction == Crypto::Cipher::Intent::Encryption) {
            auto encrypted = MUST(cipher.create_aligned_buffer(bytes.size()));
            auto encrypted_span = encrypted.bytes();

            auto iv = MUST(ByteBuffer::create_uninitialized(Crypto::Cipher::AESCipher::block_size()));
            fill_with_random(iv);

            cipher.encrypt(bytes, encrypted_span, iv);

            ByteBuffer output;
            output.append(iv);
            output.append(encrypted_span);
            assign(output);
        } else {
            VERIFY(direction == Crypto::Cipher::Intent::Decryption);

            auto iv = bytes.trim(16);
            bytes = bytes.slice(16);

            auto decrypted = MUST(cipher.create_aligned_buffer(bytes.size()));
            auto decrypted_span = decrypted.bytes();
            cipher.decrypt(bytes, decrypted_span, iv);

            assign(decrypted_span);
        }

        return;
    }

    // RC4 is symmetric, so decryption is the same as encryption.
    VERIFY(m_method == CryptFilterMethod::V2);
    RC4 rc4(key);
    auto output = rc4.encrypt(bytes);

    assign(output);
}

void StandardSecurityHandler::encrypt(NonnullRefPtr<Object> object, Reference reference) const
{
    crypt(object, reference, Crypto::Cipher::Intent::Encryption);
}

void StandardSecurityHandler::decrypt(NonnullRefPtr<Object> object, Reference reference) const
{
    crypt(object, reference, Crypto::Cipher::Intent::Decryption);
}

static constexpr auto identity_permutation = iota_array<size_t, 256>(0);

RC4::RC4(ReadonlyBytes key)
    : m_bytes(identity_permutation)
{
    size_t j = 0;
    for (size_t i = 0; i < 256; i++) {
        j = (j + m_bytes[i] + key[i % key.size()]) & 0xff;
        swap(m_bytes[i], m_bytes[j]);
    }
}

void RC4::generate_bytes(ByteBuffer& bytes)
{
    size_t i = 0;
    size_t j = 0;

    for (size_t count = 0; count < bytes.size(); count++) {
        i = (i + 1) % 256;
        j = (j + m_bytes[i]) % 256;
        swap(m_bytes[i], m_bytes[j]);
        bytes[count] = m_bytes[(m_bytes[i] + m_bytes[j]) % 256];
    }
}

ByteBuffer RC4::encrypt(ReadonlyBytes bytes)
{
    auto output = MUST(ByteBuffer::create_uninitialized(bytes.size()));
    generate_bytes(output);
    for (size_t i = 0; i < bytes.size(); i++)
        output[i] ^= bytes[i];
    return output;
}

}
