/*
 * Copyright © 2016 Lukas Rosenthaler, Andrea Bianco, Benjamin Geer,
 * Ivan Subotic, Tobias Schweizer, André Kilchenmann, and André Fatton.
 * This file is part of Sipi.
 * Sipi is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * Sipi is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * Additional permission under GNU AGPL version 3 section 7:
 * If you modify this Program, or any covered work, by linking or combining
 * it with Kakadu (or a modified version of that library) or Adobe ICC Color
 * Profiles (or a modified version of that library) or both, containing parts
 * covered by the terms of the Kakadu Software Licence or Adobe Software Licence,
 * or both, the licensors of this Program grant you additional permission
 * to convey the resulting work.
 * See the GNU Affero General Public License for more details.
 * You should have received a copy of the GNU Affero General Public
 * License along with Sipi.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <assert.h>
#include <stdlib.h>
#include <syslog.h>

#include <string>
#include <iostream>
#include <fstream>
#include <cmath>
#include <vector>
#include <cstdio>

#include <string.h>

#include "shttps/Connection.h"
#include "shttps/Global.h"

#include "SipiError.h"
#include "SipiIOJ2k.h"



// Kakadu core includes
#include "kdu_elementary.h"
#include "kdu_messaging.h"
#include "kdu_params.h"
#include "kdu_compressed.h"
#include "kdu_sample_processing.h"
// Application level includes
#include "kdu_file_io.h"
#include "kdu_stripe_decompressor.h"
#include "kdu_stripe_compressor.h"
#include "jp2.h"
#include "jpx.h"
#include "shttps/makeunique.h"

using namespace kdu_core;
using namespace kdu_supp;

static const char __file__[] = __FILE__;

namespace Sipi {

    //=========================================================================
    // Here we are implementing a subclass of kdu_core::kdu_compressed_target
    // in order to write directly to the HTTP server connection
    //
    class J2kHttpStream : public kdu_core::kdu_compressed_target {
    private:
        shttps::Connection *conobj;
    public:
        J2kHttpStream(shttps::Connection *conobj_p);

        ~J2kHttpStream();

        inline int get_capabilities() { return KDU_TARGET_CAP_SEQUENTIAL; };

        inline bool start_rewrite(kdu_long backtrack) { return false; };

        inline bool end_rewrite() { return false; };

        bool write(const kdu_byte *buf, int num_bytes);

        inline void set_target_size(kdu_long num_bytes) {}; // we just ignore it
    };
    //-------------------------------------------------------------------------

    //-------------------------------------------------------------------------
    // Constructor which takes the HTTP server connection as parameter
    //........................................................................
    J2kHttpStream::J2kHttpStream(shttps::Connection *conobj_p) : kdu_core::kdu_compressed_target() {
        conobj = conobj_p;
    };
    //-------------------------------------------------------------------------


    //-------------------------------------------------------------------------
    // Distructor which cleans up !!!!!!!!!! We still have to determine what has to be cleaned up!!!!!!!!!!
    //........................................................................
    J2kHttpStream::~J2kHttpStream() {
        // cleanup everything thats necessary...
    };
    //-------------------------------------------------------------------------

    //-------------------------------------------------------------------------
    // Write the data to the HTTP server connection
    //........................................................................
    bool J2kHttpStream::write(const kdu_byte *buf, int num_bytes) {
        try {
            conobj->sendAndFlush(buf, num_bytes);
        } catch (int i) {
            return false;
        }
        return true;
    };
    //-------------------------------------------------------------------------

    static kdu_core::kdu_byte xmp_uuid[] = {0xBE, 0x7A, 0xCF, 0xCB, 0x97, 0xA9, 0x42, 0xE8, 0x9C, 0x71, 0x99, 0x94,
                                            0x91, 0xE3, 0xAF, 0xAC};
    static kdu_core::kdu_byte iptc_uuid[] = {0x33, 0xc7, 0xa4, 0xd2, 0xb8, 0x1d, 0x47, 0x23, 0xa0, 0xba, 0xf1, 0xa3,
                                             0xe0, 0x97, 0xad, 0x38};
    static kdu_core::kdu_byte exif_uuid[] = {'J', 'p', 'g', 'T', 'i', 'f', 'f', 'E', 'x', 'i', 'f', '-', '>', 'J', 'P',
                                             '2'};
    //static kdu_core::kdu_byte geojp2_uuid[] = {0xB1, 0x4B, 0xF8, 0xBD, 0x08, 0x3D, 0x4B, 0x43, 0xA5, 0xAE, 0x8C, 0xD7, 0xD5, 0xA6, 0xCE, 0x03};
    //static kdu_core::kdu_byte world_uuid[] = {0x96, 0xa9, 0xf1, 0xf1, 0xdc, 0x98, 0x40, 0x2d, 0xa7, 0xae, 0xd6, 0x8e, 0x34, 0x45, 0x18, 0x09};



    /*!
    * Local class for handling kakadu warnings
    */
    class KduSipiWarning : public kdu_core::kdu_message {
    private:
        std::string msg;
    public:
        KduSipiWarning() : kdu_message() { msg = "KAKADU-WARNING: "; }

        KduSipiWarning(const char *lead_in) : kdu_message(), msg(lead_in) {}

        void put_text(const char *str) { msg += str; }

        void flush(bool end_of_message = false) {
            if (end_of_message) {
                syslog(LOG_WARNING, "%s", msg.c_str());
            }
        }
    };
    //=============================================================================

    /*!
    * Local class for handling kakadu errors. It overrides the "exit()" call and
    * throws a kdu_exception...
    */
    class KduSipiError : public kdu_core::kdu_message {
    private:
        std::string msg;
    public:
        KduSipiError() : kdu_message() { msg = "KAKADU-ERROR: "; }

        KduSipiError(const char *lead_in) : kdu_message(), msg(lead_in) {}

        void put_text(const char *str) { msg += str; }

        void flush(bool end_of_message = false) {
            if (end_of_message) {
                std::cerr << msg << std::endl;
                syslog(LOG_ERR, "%s", msg.c_str());
                throw KDU_ERROR_EXCEPTION;
            }
        }

        void setMsg(const std::string &msg);
    };
    //=============================================================================

    static KduSipiWarning kdu_sipi_warn("Kakadu-library: ");
    static KduSipiError kdu_sipi_error("Kakadu-library: ");

    static bool is_jpx(const char *fname) {
        int inf;
        int retval = 0;
        if ((inf = open(fname, O_RDONLY)) != -1) {
            char testbuf[48];
            char sig0[] = {'\xff', '\x52'};
            char sig1[] = {'\xff', '\x4f', '\xff', '\x51'};
            char sig2[] = {'\x00', '\x00', '\x00', '\x0C', '\x6A', '\x50', '\x20', '\x20', '\x0D', '\x0A', '\x87',
                           '\x0A'};
            auto n = read(inf, testbuf, 48);
            if ((n >= 47) && (memcmp(sig0, testbuf + 45, 2) == 0)) { retval = 1; }
            else if ((n >= 4) && (memcmp(sig1, testbuf, 4) == 0)) { retval = 1; }
            else if ((n >= 12) && (memcmp(sig2, testbuf, 12) == 0)) retval = 1;
        }
        close(inf);
        return retval == 1;
    }
    //=============================================================================


    bool SipiIOJ2k::read(SipiImage *img, std::string filepath, std::shared_ptr<SipiRegion> region,
                         std::shared_ptr<SipiSize> size, bool force_bps_8,
                         ScalingQuality scaling_quality)
    {
        if (!is_jpx(filepath.c_str())) return false; // It's not a JPGE2000....

        int num_threads;
        if ((num_threads = kdu_get_num_processors()) < 2) num_threads = 0;

        // Custom messaging services
        kdu_customize_warnings(&kdu_sipi_warn);
        kdu_customize_errors(&kdu_sipi_error);

        kdu_core::kdu_compressed_source *input = nullptr;
        kdu_supp::kdu_simple_file_source file_in;

        kdu_supp::jp2_family_src jp2_ultimate_src;
        kdu_supp::jpx_source jpx_in;
        kdu_supp::jpx_codestream_source jpx_stream;
        kdu_supp::jpx_layer_source jpx_layer;

        kdu_supp::jp2_channels channels;
        kdu_supp::jp2_palette palette;
        kdu_supp::jp2_resolution resolution;
        kdu_supp::jp2_colour colour;

        jp2_ultimate_src.open(filepath.c_str());

        if (jpx_in.open(&jp2_ultimate_src, true) < 0) { // if < 0, not compatible with JP2 or JPX.  Try opening as a raw code-stream.
            jp2_ultimate_src.close();
            file_in.open(filepath.c_str());
            input = &file_in;
        } else {
            jp2_input_box box;
            if (box.open(&jp2_ultimate_src)) {
                do {
                    if (box.get_box_type() == jp2_uuid_4cc) {
                        kdu_byte buf[16];
                        box.read(buf, 16);
                        if (memcmp(buf, xmp_uuid, 16) == 0) {
                            auto xmp_len = box.get_remaining_bytes();
                            auto xmp_buf = shttps::make_unique<char[]>(xmp_len);
                            box.read((kdu_byte *) xmp_buf.get(), xmp_len);
                            try {
                                img->xmp = std::make_shared<SipiXmp>(xmp_buf.get(),
                                                                     xmp_len); // ToDo: Problem with thread safety!!!!!!!!!!!!!!
                            } catch (SipiError &err) {
                                syslog(LOG_ERR, "%s", err.to_string().c_str());
                            }
                        } else if (memcmp(buf, iptc_uuid, 16) == 0) {
                            auto iptc_len = box.get_remaining_bytes();
                            auto iptc_buf = shttps::make_unique<unsigned char[]>(iptc_len);
                            box.read(iptc_buf.get(), iptc_len);
                            try {
                                img->iptc = std::make_shared<SipiIptc>(iptc_buf.get(), iptc_len);
                            } catch (SipiError &err) {
                                syslog(LOG_ERR, "%s", err.to_string().c_str());
                            }
                        } else if (memcmp(buf, exif_uuid, 16) == 0) {
                            auto exif_len = box.get_remaining_bytes();
                            auto exif_buf = shttps::make_unique<unsigned char[]>(exif_len);
                            box.read(exif_buf.get(), exif_len);
                            try {
                                img->exif = std::make_shared<SipiExif>(exif_buf.get(), exif_len);
                            } catch (SipiError &err) {
                                syslog(LOG_ERR, "%s", err.to_string().c_str());
                            }
                        }
                    }
                    box.close();
                } while (box.open_next());
            }

            int stream_id = 0;
            jpx_stream = jpx_in.access_codestream(stream_id);
            input = jpx_stream.open_stream();
            palette = jpx_stream.access_palette();
        }

        kdu_core::kdu_codestream codestream;
        codestream.create(input);
        //codestream.set_fussy(); // Set the parsing error tolerance.
        codestream.set_fast(); // No errors expected in input

        //
        // get SipiEssentials (if present) as codestream comment
        //
        kdu_codestream_comment comment = codestream.get_comment();
        while (comment.exists()) {
            const char *cstr = comment.get_text();
            if (strncmp(cstr, "SIPI:", 5) == 0) {
                SipiEssentials se(cstr + 5);
                img->essential_metadata(se);
                break;
            }
            comment = codestream.get_comment(comment);
        }

        //
        // get the size of the full image (without reduce!)
        //
        siz_params *siz = codestream.access_siz();
        int __nx, __ny;
        siz->get(Ssize, 0, 0, __ny);
        siz->get(Ssize, 0, 1, __nx);

        //
        // is there a region of interest defined ? If yes, get the cropping parameters...
        //
        kdu_core::kdu_dims roi;
        bool do_roi = false;
        if ((region != nullptr) && (region->getType()) != SipiRegion::FULL) {
            try {
                size_t sx, sy;
                region->crop_coords(__nx, __ny, roi.pos.x, roi.pos.y, sx, sy);
                roi.size.x = sx;
                roi.size.y = sy;
                do_roi = true;
            } catch (Sipi::SipiError &err) {
                codestream.destroy();
                input->close();
                jpx_in.close(); // Not really necessary here.
                throw err;
            }
        }

        //
        // here we prepare tha scaling/reduce stuff...
        //
        int reduce = 0;
        size_t nnx, nny;
        bool redonly = true; // we assume that only a reduce is necessary
        if ((size != nullptr) && (size->getType() != SipiSize::FULL)) {
            if (do_roi) {
                size->get_size(roi.size.x, roi.size.y, nnx, nny, reduce, redonly);
            } else {
                size->get_size(__nx, __ny, nnx, nny, reduce, redonly);
            }
        }

        if (reduce < 0) reduce = 0;
        codestream.apply_input_restrictions(0, 0, reduce, 0, do_roi ? &roi : nullptr);


        // Determine number of components to decompress
        kdu_core::kdu_dims dims;
        codestream.get_dims(0, dims);

        img->nx = dims.size.x;
        img->ny = dims.size.y;


        img->bps = codestream.get_bit_depth(0); // bitdepth of zeroth component. Assuming it's valid for all

        img->nc = codestream.get_num_components(); // not the same as the number of colors!


        //
        // The following definitions we need in case we get a palette color image!
        //
        byte *rlut = NULL;
        byte *glut = NULL;
        byte *blut = NULL;
        //
        // get ICC-Profile if available
        //
        jpx_layer = jpx_in.access_layer(0);
        img->photo = INVALID; // we initialize to an invalid value in order to test later if img->photo has been set
        int numcol;
        if (jpx_layer.exists()) {
            kdu_supp::jp2_colour colinfo = jpx_layer.access_colour(0);
            kdu_supp::jp2_channels chaninfo = jpx_layer.access_channels();
            numcol = chaninfo.get_num_colours(); // I assume these are the color channels (1, 3 or 4 in case of CMYK)
            int nluts = palette.get_num_luts();
            if (nluts == 3) {
                int nentries = palette.get_num_entries( );
                rlut = new byte[nentries];
                glut = new byte[nentries];
                blut = new byte[nentries];
                float *tmplut =  new float[nentries];

                palette.get_lut(0, tmplut);
                for (int i = 0; i < nentries; i++) {
                    rlut[i] = roundf((tmplut[i] + 0.5)*255.0);
                }

                palette.get_lut(1, tmplut);
                for (int i = 0; i < nentries; i++) {
                    glut[i] = roundf((tmplut[i] + 0.5)*255.0);
                }

                palette.get_lut(2, tmplut);
                for (int i = 0; i < nentries; i++) {
                    blut[i] = roundf((tmplut[i] + 0.5)*255.0);
                }
                delete [] tmplut;
            }

            if (img->nc > numcol) { // we have more components than colors -> alpha channel!
                for (size_t i = 0; i < img->nc - numcol; i++) { // img->nc - numcol: number of alpha channels (?)
                    img->es.push_back(ASSOCALPHA);
                }
            }
            if (colinfo.exists()) {
                int space = colinfo.get_space();
                switch (space) {
                    case kdu_supp::JP2_sRGB_SPACE: {
                        img->photo = RGB;
                        img->icc = std::make_shared<SipiIcc>(icc_sRGB);
                        break;
                    }
                    case kdu_supp::JP2_CMYK_SPACE: {
                        img->photo = SEPARATED;
                        img->icc = std::make_shared<SipiIcc>(icc_CYMK_standard);
                        break;
                    }
                    case kdu_supp::JP2_YCbCr1_SPACE: {
                        img->photo = YCBCR;
                        img->icc = std::make_shared<SipiIcc>(icc_sRGB);
                        break;
                    }
                    case kdu_supp::JP2_YCbCr2_SPACE:
                    case kdu_supp::JP2_YCbCr3_SPACE: {
                        float whitepoint[] = {0.3127, 0.3290};
                        float primaries[] = {0.630, 0.340, 0.310, 0.595, 0.155, 0.070};
                        img->photo = YCBCR;
                        img->icc = std::make_shared<SipiIcc>(whitepoint, primaries);
                        break;
                    }
                    case kdu_supp::JP2_iccRGB_SPACE: {
                        img->photo = RGB;
                        int icc_len;
                        const unsigned char *icc_buf = colinfo.get_icc_profile(&icc_len);
                        img->icc = std::make_shared<SipiIcc>(icc_buf, icc_len);
                        break;
                    }
                    case kdu_supp::JP2_iccANY_SPACE: {
                        img->photo = RGB;
                        int icc_len;
                        const unsigned char *icc_buf = colinfo.get_icc_profile(&icc_len);
                        img->icc = std::make_shared<SipiIcc>(icc_buf, icc_len);
                        break;
                    }
                    case kdu_supp::JP2_sLUM_SPACE: {
                        img->photo = MINISBLACK;
                        img->icc = std::make_shared<SipiIcc>(icc_LUM_D65);
                        break;
                    }
                    case kdu_supp::JP2_sYCC_SPACE: {
                        img->photo = YCBCR;
                        img->icc = std::make_shared<SipiIcc>(icc_sRGB);
                        break;
                    }
                    case 100: {
                        img->photo = MINISBLACK;
                        img->icc = std::make_shared<SipiIcc>(icc_ROMM_GRAY);
                        break;
                    }

                    default: {
                        std::cerr << "CS=" << space << std::endl;
                        throw SipiImageError(__file__, __LINE__, "Unsupported ICC profile: " + std::to_string(space));
                    }
                }
            }
        }
        else {
            numcol = img->nc;
        }

        if (img->photo == INVALID) {
            switch (numcol) {
                case 1: {
                    img->photo = MINISBLACK;
                    break;
                }
                case 3: {
                    img->photo = RGB;
                    break;
                }
                case 4: {
                    img->photo = SEPARATED;
                    break;
                }
                default: {
                    throw SipiImageError(__file__, __LINE__, "No meaningful photometric interpretation possible");
                }
            } // switch(numcol)
        }

        //
        // the following code directly converts a 16-Bit jpx into an 8-bit image.
        // In order to retrieve a 16-Bit image, use kdu_uin16 *buffer an the apropriate signature of the pull_stripe method
        //
        kdu_supp::kdu_stripe_decompressor decompressor;
        decompressor.start(codestream);
        int stripe_heights[4] = {dims.size.y, dims.size.y, dims.size.y,
                                 dims.size.y}; // enough for alpha channel (4 components)

        if (force_bps_8) img->bps = 8; // forces kakadu to convert to 8 bit!
        switch (img->bps) {
            case 8: {
                kdu_core::kdu_byte *buffer8 = new kdu_core::kdu_byte[(int) dims.area() * img->nc];
                decompressor.pull_stripe(buffer8, stripe_heights);
                img->pixels = (byte *) buffer8;
                break;
            }
            case 12: {
                std::vector<char> get_signed(img->nc, 0); // vector<bool> does not work -> special treatment in C++
                kdu_core::kdu_int16 *buffer16 = new kdu_core::kdu_int16[(int) dims.area() * img->nc];
                decompressor.pull_stripe(buffer16, stripe_heights, nullptr, nullptr, nullptr, nullptr, (bool *) get_signed.data());
                img->pixels = (byte *) buffer16;
                img->bps = 16;
                break;
            }
            case 16: {
                std::vector<char> get_signed(img->nc, 0); // vector<bool> does not work -> special treatment in C++
                kdu_core::kdu_int16 *buffer16 = new kdu_core::kdu_int16[(int) dims.area() * img->nc];
                decompressor.pull_stripe(buffer16, stripe_heights, nullptr, nullptr, nullptr, nullptr, (bool *) get_signed.data());
                img->pixels = (byte *) buffer16;
                break;
            }
            default: {
                decompressor.finish();
                codestream.destroy();
                input->close();
                jpx_in.close(); // Not really necessary here.
                std::cerr << "BPS=" << img->bps << std::endl;
                throw SipiImageError(__file__, __LINE__, "Unsupported number of bits/sample!");
            }
        }
        decompressor.finish();
        codestream.destroy();
        input->close();
        jpx_in.close(); // Not really necessary here.

        if (rlut != NULL) {
            //
            // we have a palette color image...
            //
            byte *tmpbuf = new byte[img->nx*img->ny*numcol];
            for (int y = 0; y < img->ny; ++y) {
                for (int x = 0; x < img->nx; ++x) {
                    tmpbuf[3*(y*img->nx + x) + 0] = rlut[img->pixels[y*img->nx + x]];
                    tmpbuf[3*(y*img->nx + x) + 1] = glut[img->pixels[y*img->nx + x]];
                    tmpbuf[3*(y*img->nx + x) + 2] = blut[img->pixels[y*img->nx + x]];
                }
            }
            delete [] img->pixels;
            img->pixels = tmpbuf;
            img->nc = numcol;
            delete [] rlut;
            delete [] glut;
            delete [] blut;
        }
        if (img->photo == YCBCR) {
            img->convertYCC2RGB();
            img->photo = RGB;
        }

        if ((size != nullptr) && (!redonly)) {
            img->scale(nnx, nny);
        }
        return true;
    }
    //=============================================================================


    bool SipiIOJ2k::getDim(std::string filepath, size_t &width, size_t &height) {
        if (!is_jpx(filepath.c_str())) return false; // It's not a JPGE2000....

        kdu_customize_warnings(&kdu_sipi_warn);
        kdu_customize_errors(&kdu_sipi_error);

        kdu_supp::jp2_family_src jp2_ultimate_src;
        kdu_supp::jpx_source jpx_in;
        kdu_supp::jpx_codestream_source jpx_stream;
        kdu_core::kdu_compressed_source *input = nullptr;
        kdu_supp::kdu_simple_file_source file_in;

        jp2_ultimate_src.open(filepath.c_str());

        if (jpx_in.open(&jp2_ultimate_src, true) <
            0) { // if < 0, not compatible with JP2 or JPX.  Try opening as a raw code-stream.
            jp2_ultimate_src.close();
            file_in.open(filepath.c_str());
            input = &file_in;
        } else {
            int stream_id = 0;
            jpx_stream = jpx_in.access_codestream(stream_id);
            input = jpx_stream.open_stream();
        }

        kdu_core::kdu_codestream codestream;
        codestream.create(input);
        codestream.set_fussy(); // Set the parsing error tolerance.

        //
        // get the size of the full image (without reduce!)
        //
        siz_params *siz = codestream.access_siz();
        int tmp_height;
        siz->get(Ssize, 0, 0, tmp_height);
        height = tmp_height;
        int tmp_width;
        siz->get(Ssize, 0, 1, tmp_width);
        width = tmp_width;

        codestream.destroy();
        input->close();
        jpx_in.close(); // Not really necessary here.

        return true;
    }
    //=============================================================================


    static void write_xmp_box(kdu_supp::jp2_family_tgt *tgt, const char *xmpstr) {
        kdu_supp::jp2_output_box out;
        out.open(tgt, jp2_uuid_4cc);
        out.set_target_size(strlen(xmpstr) + sizeof(xmp_uuid));
        out.write(xmp_uuid, 16);
        out.write((kdu_core::kdu_byte *) xmpstr, strlen(xmpstr));
        out.close();
    }
    //=============================================================================

    static void write_iptc_box(kdu_supp::jp2_family_tgt *tgt, kdu_core::kdu_byte *iptc, int iptc_len) {
        kdu_supp::jp2_output_box out;
        out.open(tgt, jp2_uuid_4cc);
        out.set_target_size(iptc_len + sizeof(iptc_uuid));
        out.write(iptc_uuid, 16);
        out.write((kdu_core::kdu_byte *) iptc, iptc_len);
        out.close();
    }
    //=============================================================================

    static void write_exif_box(kdu_supp::jp2_family_tgt *tgt, kdu_core::kdu_byte *exif, int exif_len) {
        kdu_supp::jp2_output_box out;
        out.open(tgt, jp2_uuid_4cc);
        out.set_target_size(exif_len + sizeof(exif_uuid));
        out.write(exif_uuid, sizeof(exif_uuid));
        out.write((kdu_byte *) exif, exif_len); // NOT::: skip JPEG marker header 'E', 'x', 'i', 'f', '\0', '\0'..
        out.close();
    }
    //=============================================================================


    void SipiIOJ2k::write(SipiImage *img, std::string filepath, int quality) {
        kdu_customize_warnings(&kdu_sipi_warn);
        kdu_customize_errors(&kdu_sipi_error);

        int num_threads;

        if ((num_threads = kdu_get_num_processors()) < 2) num_threads = 0;

        try {
            // Construct code-stream object
            siz_params siz;
            siz.set(Scomponents, 0, 0, (int) img->nc);
            siz.set(Sdims, 0, 0, (int) img->ny);  // Height of first image component
            siz.set(Sdims, 0, 1, (int) img->nx);   // Width of first image component
            siz.set(Sprecision, 0, 0, (int) img->bps);  // Bits per sample (usually 8 or 16)
            siz.set(Ssigned, 0, 0, false); // Image samples are originally unsigned
            kdu_params *siz_ref = &siz;
            siz_ref->finalize();

            kdu_codestream codestream;

            kdu_compressed_target *output = nullptr;
            jp2_family_tgt jp2_ultimate_tgt;
            jpx_target jpx_out;
            jpx_codestream_target jpx_stream;
            jpx_layer_target jpx_layer;
            jp2_dimensions jp2_family_dimensions;
            jp2_palette jp2_family_palette;
            jp2_resolution jp2_family_resolution;
            jp2_channels jp2_family_channels;
            jp2_colour jp2_family_colour;

            J2kHttpStream *http = nullptr;
            if (filepath == "HTTP") {
                shttps::Connection *conobj = img->connection();
                http = new J2kHttpStream(conobj);
                jp2_ultimate_tgt.open(http);
            } else {
                jp2_ultimate_tgt.open(filepath.c_str());
            }
            jpx_out.open(&jp2_ultimate_tgt);
            jpx_stream = jpx_out.add_codestream();
            jpx_layer = jpx_out.add_layer();

            jp2_family_dimensions = jpx_stream.access_dimensions();
            jp2_family_palette = jpx_stream.access_palette();
            jp2_family_resolution = jpx_layer.access_resolution();
            jp2_family_channels = jpx_layer.access_channels();
            jp2_family_colour = jpx_layer.add_colour();

            output = jpx_stream.access_stream();

            codestream.create(&siz, output);

            //
            // Custom tag for SipiEssential metadata
            //
            SipiEssentials es = img->essential_metadata();
            if (es.is_set()) {
                std::string esstr = es;
                std::string emdata = "SIPI:" + esstr;
                kdu_codestream_comment comment = codestream.add_comment();
                comment.put_text(emdata.c_str());
            }


            // Set up any specific coding parameters and finalize them.

            codestream.access_siz()->parse_string("Creversible=yes");
            codestream.access_siz()->parse_string("Clayers=8");
            codestream.access_siz()->parse_string("Clevels=8");
            codestream.access_siz()->parse_string("Corder=RPCL");
            codestream.access_siz()->parse_string("Cprecincts={256,256}");
            codestream.access_siz()->parse_string("Cblk={64,64}");
            codestream.access_siz()->parse_string("Cuse_sop=yes");
            //codestream.access_siz()->parse_string("Stiles={1024,1024}");
            //codestream.access_siz()->parse_string("ORGgen_plt=yes");
            //codestream.access_siz()->parse_string("ORGtparts=R");
            codestream.access_siz()->finalize_all(); // Set up coding defaults

            jp2_family_dimensions.init(&siz); // initalize dimension box

            if (img->icc != nullptr) {
                PredefinedProfiles icc_type = img->icc->getProfileType();
                switch (icc_type) {
                    case icc_undefined: {
                        unsigned int icc_len;
                        kdu_byte *icc_bytes = (kdu_byte *) img->icc->iccBytes(icc_len);
                        jp2_family_colour.init(icc_bytes);
                        break;
                    }
                    case icc_unknown: {
                        unsigned int icc_len;
                        kdu_byte *icc_bytes = (kdu_byte *) img->icc->iccBytes(icc_len);
                        jp2_family_colour.init(icc_bytes);
                        break;
                    }
                    case icc_sRGB: {
                        jp2_family_colour.init(JP2_sRGB_SPACE);
                        break;
                    }
                    case icc_AdobeRGB: {
                        unsigned int icc_len;
                        kdu_byte *icc_bytes = (kdu_byte *) img->icc->iccBytes(icc_len);
                        jp2_family_colour.init(icc_bytes);
                        break;
                    }
                    case icc_RGB: {
                        unsigned int icc_len;
                        kdu_byte *icc_bytes = (kdu_byte *) img->icc->iccBytes(icc_len);
                        jp2_family_colour.init(icc_bytes);
                        break;
                    }
                    case icc_CYMK_standard: {
                        jp2_family_colour.init(JP2_CMYK_SPACE);
                        break;
                    }
                    case icc_GRAY_D50: {
                        unsigned int icc_len;
                        kdu_byte *icc_bytes = (kdu_byte *) img->icc->iccBytes(icc_len);
                        jp2_family_colour.init(icc_bytes);
                        break;
                    }
                    case icc_LUM_D65: {
                        jp2_family_colour.init(JP2_sLUM_SPACE);
                        break;
                    }
                    case icc_ROMM_GRAY: {
                        jp2_family_colour.init(JP2_sLUM_SPACE);
                        break;
                    }
                    default: {
                        unsigned int icc_len;
                        kdu_byte *icc_bytes = (kdu_byte *) img->icc->iccBytes(icc_len);
                        jp2_family_colour.init(icc_bytes);
                    }
                }

            } else {
                switch (img->nc - img->es.size()) {
                    case 1: {
                        jp2_family_colour.init(JP2_sLUM_SPACE);
                        break;
                    }
                    case 3: {
                        jp2_family_colour.init(JP2_sRGB_SPACE);
                        break;
                    }
                    case 4: {
                        jp2_family_colour.init(JP2_CMYK_SPACE);
                        break;
                    }
                }
            }
            jp2_family_channels.init(img->nc - img->es.size());
            for (int c = 0; c < img->nc - img->es.size(); c++) jp2_family_channels.set_colour_mapping(c, c);
            for (int c = 0; c < img->es.size(); c++) jp2_family_channels.set_opacity_mapping(img->nc + c, img->nc + c);
            jpx_out.write_headers();

            if (img->iptc != nullptr) {
                unsigned int iptc_len = 0;
                kdu_byte *iptc_buf = img->iptc->iptcBytes(iptc_len);
                write_iptc_box(&jp2_ultimate_tgt, iptc_buf, iptc_len);
            }

            //
            // write EXIF here
            //
            if (img->exif != nullptr) {
                unsigned int exif_len = 0;
                kdu_byte *exif_buf = img->exif->exifBytes(exif_len);
                write_exif_box(&jp2_ultimate_tgt, exif_buf, exif_len);
            }

            //
            // write XMP data here
            //
            if (img->xmp != nullptr) {
                unsigned int len = 0;
                const char *xmp_buf = img->xmp->xmpBytes(len);
                if (len > 0) {
                    write_xmp_box(&jp2_ultimate_tgt, xmp_buf);
                }
            }

            //jpx_out.write_headers();
            jp2_output_box *out_box = jpx_stream.open_stream();

            codestream.access_siz()->finalize_all();

            kdu_thread_env env, *env_ref = nullptr;
            if (num_threads > 0) {
                env.create();
                for (int nt = 1; nt < num_threads; nt++) {
                    if (!env.add_thread()) num_threads = nt; // Unable to create all the threads requested
                }
                env_ref = &env;
            }


            // Now compress the image in one hit, using `kdu_stripe_compressor'
            kdu_stripe_compressor compressor;
            //compressor.start(codestream);
            compressor.start(codestream, 0, nullptr, nullptr, 0, false, false, true, 0.0, 0, false, env_ref);

            int *stripe_heights = new int[img->nc];
            for (size_t i = 0; i < img->nc; i++) {
                stripe_heights[i] = img->ny;
            }

            int *precisions;
            bool *is_signed;

            if (img->bps == 16) {
                kdu_int16 *buf = (kdu_int16 *) img->pixels;
                precisions = new int[img->nc];
                is_signed = new bool[img->nc];
                for (size_t i = 0; i < img->nc; i++) {
                    precisions[i] = img->bps;
                    is_signed[i] = false;
                }
                compressor.push_stripe(buf, stripe_heights, nullptr, nullptr, nullptr, precisions, is_signed);
            } else if (img->bps == 8) {
                kdu_byte *buf = (kdu_byte *) img->pixels;
                compressor.push_stripe(buf, stripe_heights);
            } else {
                throw SipiImageError(__file__, __LINE__, "Unsupported number of bits/sample!");
            }
            compressor.finish();

            // Finally, cleanup
            codestream.destroy(); // All done: simple as that.
            output->close(); // Not really necessary here.
            jpx_out.close();
            if (jp2_ultimate_tgt.exists()) {
                jp2_ultimate_tgt.close();
            }

            delete[] stripe_heights;
            if (img->bps == 16) {
                delete[] precisions;
                delete[] is_signed;
            }

            delete http;
        } catch (kdu_exception e) {
            throw SipiImageError(__file__, __LINE__, "Problem writing a JPEG2000 image!");
        }
        return;
    }
} // namespace Sipi
