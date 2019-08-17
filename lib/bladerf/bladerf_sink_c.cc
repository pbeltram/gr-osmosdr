/* -*- c++ -*- */
/*
 * Copyright 2013-2017 Nuand LLC
 * Copyright 2013 Dimitri Stolnikov <horiz0n@gmx.net>
 *
 * GNU Radio is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3, or (at your option)
 * any later version.
 *
 * GNU Radio is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with GNU Radio; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 */

/*
 * config.h is generated by configure.  It contains the results
 * of probing for features, options etc.  It should be the first
 * file included in your .cc file.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <iostream>

#include <boost/assign.hpp>
#include <boost/format.hpp>
#include <boost/lexical_cast.hpp>

#include <gnuradio/io_signature.h>

#include <volk/volk.h>

#include "arg_helpers.h"
#include "bladerf_sink_c.h"
#include "osmosdr/sink.h"

using namespace boost::assign;

/******************************************************************************
 * Functions
 ******************************************************************************/

/*
 * Create a new instance of bladerf_sink_c and return
 * a boost shared_ptr.  This is effectively the public constructor.
 */
bladerf_sink_c_sptr make_bladerf_sink_c(const std::string &args)
{
  return gnuradio::get_initial_sptr(new bladerf_sink_c(args));
}

/******************************************************************************
 * Private methods
 ******************************************************************************/

/*
 * The private constructor
 */
bladerf_sink_c::bladerf_sink_c(const std::string &args) :
  gr::sync_block( "bladerf_sink_c",
                  args_to_io_signature(args),
                  gr::io_signature::make(0, 0, 0)),
  _16icbuf(NULL),
  _32fcbuf(NULL),
  _in_burst(false),
  _running(false)
{
  dict_t dict = params_to_dict(args);

  /* Perform src/sink agnostic initializations */
  init(dict, BLADERF_TX);

  /* Check for RX-only params */
  if (dict.count("loopback")) {
    BLADERF_WARNING("Warning: 'loopback' has been specified on a bladeRF "
                    "sink, and will have no effect. This parameter should be "
                    "specified on the associated bladeRF source.");
  }

  if (dict.count("rxmux")) {
    BLADERF_WARNING("Warning: 'rxmux' has been specified on a bladeRF sink, "
                    "and will have no effect.");
  }

  /* Bias tee */
  if (dict.count("biastee")) {
    set_biastee_mode(dict["biastee"]);
  }

  /* Initialize channel <-> antenna map */
  BOOST_FOREACH(std::string ant, get_antennas()) {
    _chanmap[str2channel(ant)] = -1;
  }

  /* Bounds-checking output signature depending on our underlying hardware */
  if (get_num_channels() > get_max_channels()) {
    BLADERF_WARNING("Warning: number of channels specified on command line ("
                    << get_num_channels() << ") is greater than the maximum "
                    "number supported by this device (" << get_max_channels()
                    << "). Resetting to " << get_max_channels() << ".");

    set_input_signature(gr::io_signature::make(get_max_channels(),
                                               get_max_channels(),
                                               sizeof(gr_complex)));
  }

  /* Set up constraints */
  int const alignment_multiple = volk_get_alignment() / sizeof(gr_complex);
  set_alignment(std::max(1,alignment_multiple));
  set_max_noutput_items(_samples_per_buffer);
  set_output_multiple(get_num_channels());

  /* Set channel layout */
  _layout = (get_num_channels() > 1) ? BLADERF_TX_X2 : BLADERF_TX_X1;

  /* Initial wiring of antennas to channels */
  for (size_t ch = 0; ch < get_num_channels(); ++ch) {
    set_channel_enable(BLADERF_CHANNEL_TX(ch), true);
    _chanmap[BLADERF_CHANNEL_TX(ch)] = ch;
  }

  BLADERF_DEBUG("initialization complete");
}


/******************************************************************************
 * Public methods
 ******************************************************************************/

std::string bladerf_sink_c::name()
{
  return "bladeRF transmitter";
}

std::vector<std::string> bladerf_sink_c::get_devices()
{
  return bladerf_common::devices();
}

size_t bladerf_sink_c::get_max_channels()
{
  return bladerf_common::get_max_channels(BLADERF_TX);
}

size_t bladerf_sink_c::get_num_channels()
{
  return input_signature()->max_streams();
}

bool bladerf_sink_c::start()
{
  int status;

  BLADERF_DEBUG("starting sink");

  gr::thread::scoped_lock guard(d_mutex);

  _in_burst = false;

  status = bladerf_sync_config(_dev.get(), _layout, _format, _num_buffers,
                               _samples_per_buffer, _num_transfers,
                               _stream_timeout);
  if (status != 0) {
    BLADERF_THROW_STATUS(status, "bladerf_sync_config failed");
  }

  for (size_t ch = 0; ch < get_max_channels(); ++ch) {
    bladerf_channel brfch = BLADERF_CHANNEL_TX(ch);
    status = bladerf_enable_module(_dev.get(), brfch, get_channel_enable(brfch));
    if (status != 0) {
      BLADERF_THROW_STATUS(status, "bladerf_enable_module failed");
    }
  }

  /* Allocate memory for conversions in work() */
  size_t alignment = volk_get_alignment();

  _16icbuf = reinterpret_cast<int16_t *>(volk_malloc(2*_samples_per_buffer*sizeof(int16_t), alignment));
  _32fcbuf = reinterpret_cast<gr_complex *>(volk_malloc(_samples_per_buffer*sizeof(gr_complex), alignment));

  _running = true;

  return true;
}

bool bladerf_sink_c::stop()
{
  int status;

  BLADERF_DEBUG("stopping sink");

  gr::thread::scoped_lock guard(d_mutex);

  if (!_running) {
    BLADERF_WARNING("sink already stopped, nothing to do here");
    return true;
  }

  _running = false;

  for (size_t ch = 0; ch < get_max_channels(); ++ch) {
    bladerf_channel brfch = BLADERF_CHANNEL_TX(ch);
    status = bladerf_enable_module(_dev.get(), brfch, get_channel_enable(brfch));
    if (status != 0) {
      BLADERF_THROW_STATUS(status, "bladerf_enable_module failed");
    }
  }

  /* Deallocate conversion memory */
  volk_free(_16icbuf);
  volk_free(_32fcbuf);
  _16icbuf = NULL;
  _32fcbuf = NULL;

  return true;
}

int bladerf_sink_c::work(int noutput_items,
                          gr_vector_const_void_star &input_items,
                          gr_vector_void_star &output_items)
{
  int status;
  size_t nstreams = num_streams(_layout);

  gr::thread::scoped_lock guard(d_mutex);

  // if we aren't running, nothing to do here
  if (!_running) {
    return 0;
  }

  // copy the samples from input_items
  gr_complex const **in = reinterpret_cast<gr_complex const **>(&input_items[0]);

  if (nstreams > 1) {
    // we need to interleave the streams as we copy
    gr_complex *intl_out = _32fcbuf;

    for (size_t i = 0; i < (noutput_items/nstreams); ++i) {
      for (size_t n = 0; n < nstreams; ++n) {
        memcpy(intl_out++, in[n]++, sizeof(gr_complex));
      }
    }
  } else {
    // no interleaving to do: simply copy everything
    memcpy(_32fcbuf, in[0], noutput_items * sizeof(gr_complex));
  }

  // convert floating point to fixed point and scale
  // input_items is gr_complex (2x float), so num_points is 2*noutput_items
  volk_32f_s32f_convert_16i(_16icbuf, reinterpret_cast<float const *>(_32fcbuf),
                            SCALING_FACTOR, 2*noutput_items);

  // transmit the samples from the temp buffer
  if (BLADERF_FORMAT_SC16_Q11_META == _format) {
    status = transmit_with_tags(_16icbuf, noutput_items);
  } else {
    status = bladerf_sync_tx(_dev.get(), static_cast<void const *>(_16icbuf),
                             noutput_items, NULL, _stream_timeout);
  }

  // handle failure
  if (status != 0) {
    BLADERF_WARNING("bladerf_sync_tx error: " << bladerf_strerror(status));
    ++_failures;

    if (_failures >= MAX_CONSECUTIVE_FAILURES) {
      BLADERF_WARNING("Consecutive error limit hit. Shutting down.");
      return WORK_DONE;
    }
  } else {
    _failures = 0;
  }

  return noutput_items;
}

int bladerf_sink_c::transmit_with_tags(int16_t const *samples,
                                        int noutput_items)
{
  int status;
  int count = 0;

  // For a long burst, we may be transmitting the burst contents over
  // multiple work calls, so we'll just be sending the entire buffer
  // Therefore, we initialize our indicies for this case.
  int start_idx = 0;
  int end_idx = (noutput_items - 1);

  struct bladerf_metadata meta;
  std::vector<gr::tag_t> tags;

  int const INVALID_IDX = -1;
  int16_t const zeros[8] = { 0 };

  memset(&meta, 0, sizeof(meta));

  BLADERF_DEBUG("transmit_with_tags(" << noutput_items << ")");

  // Important Note: We assume that these tags are ordered by their offsets.
  // This is true for GNU Radio 3.7.7.x, since the GR runtime libs store
  // these in a multimap.
  //
  // If you're using an earlier GNU Radio version, you may have to sort
  // the tags vector.
  get_tags_in_window(tags, 0, 0, noutput_items);

  if (tags.size() == 0) {
    if (_in_burst) {
      BLADERF_DEBUG("TX'ing " << noutput_items << " samples within a burst...");

      return bladerf_sync_tx(_dev.get(), samples, noutput_items,
                             &meta, _stream_timeout);
    } else {
      BLADERF_WARNING("Dropping " << noutput_items
                      << " samples not in a burst.");
    }
  }

  BOOST_FOREACH(gr::tag_t tag, tags) {
    // Upon seeing an SOB tag, update our offset. We'll TX the start of the
    // burst when we see an EOB or at the end of this function - whichever
    // occurs first.
    if (pmt::symbol_to_string(tag.key) == "tx_sob") {
      if (_in_burst) {
        BLADERF_WARNING("Got SOB while already within a burst");

        return BLADERF_ERR_INVAL;
      } else {
        start_idx = static_cast<int>(tag.offset - nitems_read(0));

        BLADERF_DEBUG("Got SOB " << start_idx << " samples into work payload");

        meta.flags |= (BLADERF_META_FLAG_TX_NOW | BLADERF_META_FLAG_TX_BURST_START);
        _in_burst = true;
      }

    } else if (pmt::symbol_to_string(tag.key) == "tx_eob") {
      if (!_in_burst) {
        BLADERF_WARNING("Got EOB while not in burst");
        return BLADERF_ERR_INVAL;
      }

      // Upon seeing an EOB, transmit what we have and reset our state
      end_idx = static_cast<int>(tag.offset - nitems_read(0));
      BLADERF_DEBUG("Got EOB " << end_idx << " samples into work payload");

      if ((start_idx == INVALID_IDX) || (start_idx > end_idx)) {
        BLADERF_DEBUG("Buffer indicies are in an invalid state!");
        return BLADERF_ERR_INVAL;
      }

      count = end_idx - start_idx + 1;

      BLADERF_DEBUG("TXing @ EOB [" << start_idx << ":" << end_idx << "]");

      status = bladerf_sync_tx(_dev.get(),
                               static_cast<void const *>(&samples[2 * start_idx]),
                               count, &meta, _stream_timeout);
      if (status != 0) {
        return status;
      }

      /* TODO: libbladeRF should now take care of this for us,
       *       as of the libbladeRF version that includes the
       *       TX_UPDATE_TIMESTAMP flag.  Verify this potentially remove this.
       *       (The meta.flags changes would then be applied to the previous
       *       bladerf_sync_tx() call.)
       */
      BLADERF_DEBUG("TXing Zeros with burst end flag");

      meta.flags &= ~(BLADERF_META_FLAG_TX_NOW | BLADERF_META_FLAG_TX_BURST_START);
      meta.flags |= BLADERF_META_FLAG_TX_BURST_END;

      status = bladerf_sync_tx(_dev.get(), static_cast<void const *>(zeros),
                               4, &meta, _stream_timeout);

      /* Reset our state */
      start_idx = INVALID_IDX;
      end_idx = (noutput_items - 1);
      meta.flags = 0;
      _in_burst = false;

      if (status != 0) {
        BLADERF_DEBUG("Failed to send zero samples to flush EOB");
        return status;
      }
    }
  }

  // We had a start of burst with no end yet - transmit those samples
  if (_in_burst) {
    count = end_idx - start_idx + 1;

    BLADERF_DEBUG("TXing SOB [" << start_idx << ":" << end_idx << "]");

    status = bladerf_sync_tx(_dev.get(),
                             static_cast<void const *>(&samples[2 * start_idx]),
                             count, &meta, _stream_timeout);
  }

  return status;
}

osmosdr::meta_range_t bladerf_sink_c::get_sample_rates()
{
  return sample_rates(chan2channel(BLADERF_TX, 0));
}

double bladerf_sink_c::set_sample_rate(double rate)
{
  return bladerf_common::set_sample_rate(rate, chan2channel(BLADERF_TX, 0));
}

double bladerf_sink_c::get_sample_rate()
{
  return bladerf_common::get_sample_rate(chan2channel(BLADERF_TX, 0));
}

osmosdr::freq_range_t bladerf_sink_c::get_freq_range(size_t chan)
{
  return bladerf_common::freq_range(chan2channel(BLADERF_TX, chan));
}

double bladerf_sink_c::set_center_freq(double freq, size_t chan)
{
  return bladerf_common::set_center_freq(freq, chan2channel(BLADERF_TX, chan));
}

double bladerf_sink_c::get_center_freq(size_t chan)
{
  return bladerf_common::get_center_freq(chan2channel(BLADERF_TX, chan));
}

double bladerf_sink_c::set_freq_corr(double ppm, size_t chan)
{
  /* TODO: Write the VCTCXO with a correction value (also changes RX ppm value!) */
  BLADERF_WARNING("Frequency correction is not implemented.");
  return get_freq_corr(chan2channel(BLADERF_TX, chan));
}

double bladerf_sink_c::get_freq_corr(size_t chan)
{
  /* TODO: Return back the frequency correction in ppm */
  return 0;
}

std::vector<std::string> bladerf_sink_c::get_gain_names(size_t chan)
{
  return bladerf_common::get_gain_names(chan2channel(BLADERF_TX, chan));
}

osmosdr::gain_range_t bladerf_sink_c::get_gain_range(size_t chan)
{
  return bladerf_common::get_gain_range(chan2channel(BLADERF_TX, chan));
}

osmosdr::gain_range_t bladerf_sink_c::get_gain_range(const std::string &name,
                                                     size_t chan)
{
  return bladerf_common::get_gain_range(name, chan2channel(BLADERF_TX, chan));
}

bool bladerf_sink_c::set_gain_mode(bool automatic, size_t chan)
{
  return bladerf_common::set_gain_mode(automatic,
                                       chan2channel(BLADERF_TX, chan));
}

bool bladerf_sink_c::get_gain_mode(size_t chan)
{
  return bladerf_common::get_gain_mode(chan2channel(BLADERF_TX, chan));
}

double bladerf_sink_c::set_gain(double gain, size_t chan)
{
  return bladerf_common::set_gain(gain, chan2channel(BLADERF_TX, chan));
}

double bladerf_sink_c::set_gain(double gain, const std::string &name,
                                size_t chan)
{
  return bladerf_common::set_gain(gain, name, chan2channel(BLADERF_TX, chan));
}

double bladerf_sink_c::get_gain(size_t chan)
{
  return bladerf_common::get_gain(chan2channel(BLADERF_TX, chan));
}

double bladerf_sink_c::get_gain(const std::string &name, size_t chan)
{
  return bladerf_common::get_gain(name, chan2channel(BLADERF_TX, chan));
}

std::vector<std::string> bladerf_sink_c::get_antennas(size_t chan)
{
  return bladerf_common::get_antennas(BLADERF_TX);
}

std::string bladerf_sink_c::set_antenna(const std::string &antenna,
                                        size_t chan)
{
  bool _was_running = _running;

  if (_was_running) {
    stop();
  }

  bladerf_common::set_antenna(BLADERF_TX, chan, antenna);

  if (_was_running) {
    start();
  }

  return get_antenna(chan);
}

std::string bladerf_sink_c::get_antenna(size_t chan)
{
  return channel2str(chan2channel(BLADERF_TX, chan));
}

void bladerf_sink_c::set_dc_offset(const std::complex < double > &offset,
                                   size_t chan)
{
  int status;

  status = bladerf_common::set_dc_offset(offset, chan2channel(BLADERF_TX, chan));

  if (status != 0) {
    BLADERF_THROW_STATUS(status, "could not set dc offset");
  }
}

void bladerf_sink_c::set_iq_balance(const std::complex < double > &balance,
                                    size_t chan)
{
  int status;

  status = bladerf_common::set_iq_balance(balance, chan2channel(BLADERF_TX, chan));

  if (status != 0) {
    BLADERF_THROW_STATUS(status, "could not set iq balance");
  }
}

osmosdr::freq_range_t bladerf_sink_c::get_bandwidth_range(size_t chan)
{
  return filter_bandwidths(chan2channel(BLADERF_TX, chan));
}

double bladerf_sink_c::set_bandwidth(double bandwidth, size_t chan)
{
  return bladerf_common::set_bandwidth(bandwidth, chan2channel(BLADERF_TX, chan));
}

double bladerf_sink_c::get_bandwidth(size_t chan)
{
  return bladerf_common::get_bandwidth(chan2channel(BLADERF_TX, chan));
}

std::vector < std::string > bladerf_sink_c::get_clock_sources(size_t mboard)
{
  return bladerf_common::get_clock_sources(mboard);
}

void bladerf_sink_c::set_clock_source(const std::string &source,
                                      size_t mboard)
{
  bladerf_common::set_clock_source(source, mboard);
}

std::string bladerf_sink_c::get_clock_source(size_t mboard)
{
  return bladerf_common::get_clock_source(mboard);
}

void bladerf_sink_c::set_biastee_mode(const std::string &mode)
{
  int status;
  bool enable;

  if (mode == "on" || mode == "1" || mode == "rx") {
    enable = true;
  } else {
    enable = false;
  }

  status = bladerf_set_bias_tee(_dev.get(), BLADERF_CHANNEL_TX(0), enable);
  if (BLADERF_ERR_UNSUPPORTED == status) {
    // unsupported, but not worth crashing out
    BLADERF_WARNING("Bias-tee not supported by device");
  } else if (status != 0) {
    BLADERF_THROW_STATUS(status, "Failed to set bias-tee");
  }
}
