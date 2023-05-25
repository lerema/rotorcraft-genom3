/*
 * Copyright (c) 2015-2023 LAAS/CNRS
 * All rights reserved.
 *
 * Redistribution and use  in source  and binary  forms,  with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *   1. Redistributions of  source  code must retain the  above copyright
 *      notice and this list of conditions.
 *   2. Redistributions in binary form must reproduce the above copyright
 *      notice and  this list of  conditions in the  documentation and/or
 *      other materials provided with the distribution.
 *
 *					Anthony Mallet on Fri Feb 13 2015
 */
#include "acrotorcraft.h"

#include <sys/time.h>
#include <err.h>
#include <float.h>
#include <math.h>
#include <time.h>
#include <unistd.h>

#include "rotorcraft_c_types.h"
#include "codels.h"


/* --- Task main -------------------------------------------------------- */

/** Codel mk_main_init of task main.
 *
 * Triggered by rotorcraft_start.
 * Yields to rotorcraft_main.
 */
genom_event
mk_main_init(rotorcraft_ids *ids, const rotorcraft_imu *imu,
             const rotorcraft_mag *mag, const genom_context self)
{
  struct timeval tv;
  genom_event e;
  size_t i;

  gettimeofday(&tv, NULL);

  ids->conn = malloc(sizeof(*ids->conn));
  if (!ids->conn) return mk_e_sys_error(NULL, self);
  *ids->conn = (rotorcraft_conn_s){ .chan = NULL, .n = 0 };

  ids->sensor_time = (rotorcraft_ids_sensor_time_s){
    .rate = { .imu = 1000., .mag = 100., .motor = 100., .battery = 1. }
  };
  ids->imu_filter = (rotorcraft_ids_imu_filter_s){
    .galpha = { 1., 1., 1. },
    .aalpha = { 1., 1., 1. },
    .malpha = { 1., 1., 1. },

    .g = { nan(""), nan(""), nan("") },
    .a = { nan(""), nan(""), nan("") },
    .m = { nan(""), nan(""), nan("") },

    .gf = { nan(""), nan(""), nan("") },
    .af = { nan(""), nan(""), nan("") },
    .mf = { nan(""), nan(""), nan("") }
  };

  e = mk_set_sensor_rate(&ids->sensor_time.rate,
                         ids->conn, &ids->imu_filter, &ids->sensor_time, self);
  if (e) return e;

  ids->publish_time = (rotorcraft_ids_publish_time_s){ 0 };
  ids->log_time = (rotorcraft_ids_publish_time_s){ 0 } ;

  ids->imu_temp = nan("");

  ids->battery = (rotorcraft_ids_battery_s){
    .ts = { .sec = tv.tv_sec, .nsec = tv.tv_usec * 1000 },
    .min = 14.0, .max = 16.8,
    .level = nan("")
  };

  ids->calib_param = (rotorcraft_ids_calibration_param_s){
    .motion_tolerance = 10.
  };
  ids->imu_calibration = (rotorcraft_ids_imu_calibration_s){
    .gscale = {
      1., 0., 0.,
      0., 1., 0.,
      0., 0., 1.
    },
    .gbias = { 0., 0., 0. },
    .gstddev = { 1e-2, 1e-2, 1e-2 },

    .ascale = {
      1., 0., 0.,
      0., 1., 0.,
      0., 0., 1.
    },
    .abias = { 0., 0., 0. },
    .astddev = { 5e-2, 5e-2, 5e-2 },

    .mscale = {
      1., 0., 0.,
      0., 1., 0.,
      0., 0., 1.
    },
    .mbias = { 0., 0., 0. },
    .mstddev = { 5e-2, 5e-2, 5e-2 },
  };
  ids->imu_calibration_updated = true;

  for(i = 0; i < or_rotorcraft_max_rotors; i++) {
    ids->rotor_data[i] = (rotorcraft_ids_rotor_data_s){
      .state = {
        .ts = { .sec = tv.tv_sec, .nsec = tv.tv_usec * 1000 },
        .emerg = false, .spinning = false, .starting = false, .disabled = true,
        .velocity = nan(""), .throttle = nan(""), .consumption = nan(""),
        .energy_level = nan("")
      },
      .wd = 0.,
      .clkrate = 0,
      .autoconf = true
    };
  }

  ids->servo.timeout = 30.;
  ids->servo.ramp = 3.;

  /* init logging */
  ids->log = malloc(sizeof(*ids->log));
  if (!ids->log) abort();
  *ids->log = (rotorcraft_log_s){
    .fd = -1,
    .req = {
      .aio_fildes = -1,
      .aio_offset = 0,
      .aio_buf = ids->log->buffer,
      .aio_nbytes = 0,
      .aio_reqprio = 0,
      .aio_sigevent = { .sigev_notify = SIGEV_NONE },
      .aio_lio_opcode = LIO_NOP
    },
    .pending = false, .skipped = false,
    .decimation = 1, .missed = 0, .total = 0
  };

  *imu->data(self) = *mag->data(self) = (or_pose_estimator_state){
    .ts = { .sec = tv.tv_sec, .nsec = tv.tv_usec * 1000 },
    .intrinsic = true,

    .pos._present = false,
    .att._present = false,
    .pos_cov._present = false,
    .att_cov._present = false,
    .att_pos_cov._present = false,

    .vel._present = false,
    .vel_cov._present = false,
    .avel._present = false,
    .avel_cov._present = false,

    .acc._present = false,
    .acc_cov._present = false,
    .aacc._present = false,
    .aacc_cov._present = false
  };

  return rotorcraft_main;
}


/** Codel mk_main_perm of task main.
 *
 * Triggered by rotorcraft_main.
 * Yields to rotorcraft_log.
 */
genom_event
mk_main_perm(const rotorcraft_conn_s *conn,
             const rotorcraft_ids_battery_s *battery,
             const rotorcraft_ids_imu_calibration_s *imu_calibration,
             const rotorcraft_ids_rotor_data_s rotor_data[8],
             rotorcraft_ids_sensor_time_s *sensor_time,
             rotorcraft_ids_publish_time_s *publish_time,
             bool *imu_calibration_updated,
             const rotorcraft_rotor_measure *rotor_measure,
             const rotorcraft_imu *imu, const rotorcraft_mag *mag,
             const genom_context self)
{
  or_pose_estimator_state *idata = imu->data(self);
  or_pose_estimator_state *mdata = mag->data(self);
  or_rotorcraft_output *rdata = rotor_measure->data(self);
  struct timeval tv;
  ssize_t i;

  gettimeofday(&tv, NULL);

  /* battery level */
  if (!isnan(battery->level) &&
      battery->level > 0. && battery->level < battery->min) {
    static int cnt;
    uint32_t i;

    for(i = 0; i < conn->n; i++)
      if (!cnt) mk_send_msg(&conn->chan[i], "~%2", 440);

    cnt = (cnt + 1) % 500;
  }

  /* imu covariance data */
  if (*imu_calibration_updated) {
    idata->avel_cov._value.cov[0] =
      imu_calibration->gstddev[0] * imu_calibration->gstddev[0];
    idata->avel_cov._value.cov[1] = 0.;
    idata->avel_cov._value.cov[2] =
      imu_calibration->gstddev[1] * imu_calibration->gstddev[1];
    idata->avel_cov._value.cov[3] = 0.;
    idata->avel_cov._value.cov[4] = 0.;
    idata->avel_cov._value.cov[5] =
      imu_calibration->gstddev[2] * imu_calibration->gstddev[2];
    idata->avel_cov._present = true;

    idata->acc_cov._value.cov[0] =
      imu_calibration->astddev[0] * imu_calibration->astddev[0];
    idata->acc_cov._value.cov[1] = 0.;
    idata->acc_cov._value.cov[2] =
      imu_calibration->astddev[1] * imu_calibration->astddev[1];
    idata->acc_cov._value.cov[3] = 0.;
    idata->acc_cov._value.cov[4] = 0.;
    idata->acc_cov._value.cov[5] =
      imu_calibration->astddev[2] * imu_calibration->astddev[2];
    idata->acc_cov._present = true;

    mdata->att_cov._value.cov[0] = 0.;
    mdata->att_cov._value.cov[1] = 0.;
    mdata->att_cov._value.cov[2] =
      imu_calibration->mstddev[0] * imu_calibration->mstddev[0];
    mdata->att_cov._value.cov[3] = 0.;
    mdata->att_cov._value.cov[4] = 0.;
    mdata->att_cov._value.cov[5] =
      imu_calibration->mstddev[1] * imu_calibration->mstddev[1];
    mdata->att_cov._value.cov[6] = 0.;
    mdata->att_cov._value.cov[7] = 0.;
    mdata->att_cov._value.cov[8] = 0.;
    mdata->att_cov._value.cov[9] =
      imu_calibration->mstddev[2] * imu_calibration->mstddev[2];
    mdata->att_cov._present = true;

    *imu_calibration_updated = false;
  }

  /* publish, only if timestamps changed */
  if (rc_neqexts(publish_time->imu, idata->ts))
    imu->write(self);

  if (rc_neqexts(publish_time->mag, mdata->ts))
    mag->write(self);

  for(i = 0; i < or_rotorcraft_max_rotors; i++) {
    rdata->rotor._buffer[i] = rotor_data[i].state;
    if (!rotor_data[i].state.disabled)
      rdata->rotor._length = i + 1;
  }

  for(i = 0; i < or_rotorcraft_max_rotors; i++) {
    if (rc_neqexts(publish_time->mstate[i], rotor_data[i].state.ts)) {
      rotor_measure->write(self);
      for(; i < or_rotorcraft_max_rotors; i++)
        publish_time->mstate[i] = rotor_data[i].state.ts;
      break;
    }
  }


  /* update sensor time */
  {
    double rate;

    rate = 1./ (
      tv.tv_sec - idata->ts.sec +
      (1 + tv.tv_usec * 1000 - idata->ts.nsec) * 1e-9)	;
    if (rate < 0.1 * sensor_time->rate.imu || sensor_time->rate.imu < 0.1)
      sensor_time->measured_rate.imu = 0.;

    rate = 1./ (
      tv.tv_sec - mdata->ts.sec +
      (1 + tv.tv_usec * 1000 - mdata->ts.nsec) * 1e-9)	;
    if (rate < 0.1 * sensor_time->rate.mag || sensor_time->rate.mag < 0.1)
      sensor_time->measured_rate.mag = 0.;

    for(i = 0; i < or_rotorcraft_max_rotors; i++) {
      if (rotor_data[i].state.disabled) continue;

      rate = 1. / (
        tv.tv_sec - rotor_data[i].state.ts.sec +
        (1 + tv.tv_usec * 1000 - rotor_data[i].state.ts.nsec) * 1e-9);
      if (rate < 0.1 * sensor_time->rate.motor ||
          sensor_time->rate.motor < 0.1) {
        sensor_time->measured_rate.motor = 0.;
      }
    }
  }

  return rotorcraft_log;
}


/** Codel rc_main_log of task main.
 *
 * Triggered by rotorcraft_log.
 * Yields to rotorcraft_pause_main.
 */
genom_event
rc_main_log(const rotorcraft_ids_battery_s *battery, double imu_temp,
            const rotorcraft_ids_rotor_data_s rotor_data[8],
            const rotorcraft_ids_sensor_time_s_rate_s *measured_rate,
            const rotorcraft_rotor_measure *rotor_measure,
            const rotorcraft_imu *imu, const rotorcraft_mag *mag,
            const rotorcraft_ids_imu_filter_s *imu_filter,
            rotorcraft_ids_publish_time_s *log_time,
            rotorcraft_log_s **log, const genom_context self)
{
  or_pose_estimator_state *idata = imu->data(self);
  or_pose_estimator_state *mdata = mag->data(self);
  or_rotorcraft_output *rdata = rotor_measure->data(self);
  struct timeval tv;
  int i;

  if ((*log)->req.aio_fildes < 0) return rotorcraft_pause_main;

  (*log)->total++;
  if ((*log)->total % (*log)->decimation) return rotorcraft_pause_main;

  if ((*log)->pending) {
    switch (aio_error(&(*log)->req)) {
      case EINPROGRESS:
        (*log)->skipped = true;
        (*log)->missed++;
        return rotorcraft_pause_main;
    }

    (*log)->pending = false;
    if (aio_return(&(*log)->req) <= 0) {
      warn("log");
      mk_log_stop(log, self);
      return rotorcraft_pause_main;
    }
  }

  gettimeofday(&tv, NULL);

  /* build log buffer */
  const char * const end = &(*log)->buffer[sizeof((*log)->buffer)];
  char *p = (*log)->buffer;

#define xprint(...)                                                     \
  do {                                                                  \
    int s = snprintf(p, end-p, __VA_ARGS__);                            \
    p += s;                                                             \
    if (s < 0 || p >= end) goto full;                                   \
  } while(0)

  if ((*log)->skipped) xprint("\n");
  /* ts */
  xprint("%"PRIu64".%09d ", (uint64_t)tv.tv_sec, (uint32_t)tv.tv_usec*1000);

  /* rate */
  xprint(" %g %g %g ",
         measured_rate->imu, measured_rate->mag, measured_rate->motor);

  /* bat */
  if (rc_neqexts(log_time->battery, battery->ts))
    xprint(" %g ", battery->level);
  else
    xprint(" - ");

  /* imu */
  if (rc_neqexts(log_time->imu, idata->ts))
    xprint(
      " %g  %g %g %g  %g %g %g  %g %g %g  %g %g %g ",
      imu_temp,
      idata->avel._value.wx, idata->avel._value.wy, idata->avel._value.wz,
      imu_filter->g[0], imu_filter->g[1], imu_filter->g[2],
      idata->acc._value.ax, idata->acc._value.ay, idata->acc._value.az,
      imu_filter->a[0], imu_filter->a[1], imu_filter->a[2]);
  else
    xprint(" -  - - -  - - -  - - -  - - - ");

  /* mag */
  if (rc_neqexts(log_time->mag, mdata->ts))
    xprint(
      " %g %g %g  %g %g %g ",
      mdata->att._value.qx, mdata->att._value.qy, mdata->att._value.qz,
      imu_filter->m[0], imu_filter->m[1], imu_filter->m[2]);
  else
    xprint(" - - -  - - - ");

  /* cmd */
  for(i = 0; i < or_rotorcraft_max_rotors; i++) {
    if (rc_neqexts(log_time->mwd[i], rotor_data[i].ts))
      xprint(" %g", rotor_data[i].wd);
    else
      xprint(" -");
  }

  /* meas */
  for(i = 0; i < or_rotorcraft_max_rotors; i++) {
    if (rc_neqexts(log_time->mstate[i], rdata->rotor._buffer[i].ts))
      xprint(" %g", rdata->rotor._buffer[i].velocity);
    else
      xprint(" -");
  }

  /* clk */
  for(i = 0; i < or_rotorcraft_max_rotors; i++)
    xprint(" %d", rotor_data[i].clkrate);

  xprint("\n");
#undef xprint

  (*log)->req.aio_nbytes = p - (*log)->buffer;
  if (aio_write(&(*log)->req)) {
    warn("log");
    goto err;
  }

  (*log)->pending = true;
  (*log)->skipped = false;

  return rotorcraft_pause_main;
full:
  warnx("log buffer overflow");
err:
  mk_log_stop(log, self);
  return rotorcraft_pause_main;
}


/** Codel mk_main_stop of task main.
 *
 * Triggered by rotorcraft_stop.
 * Yields to rotorcraft_ether.
 */
genom_event
mk_main_stop(rotorcraft_log_s **log, const genom_context self)
{
  mk_log_stop(log, self);
  if (*log) free(*log);

  return rotorcraft_ether;
}


/* --- Activity calibrate_imu ------------------------------------------- */

/** Codel mk_calibrate_imu_start of activity calibrate_imu.
 *
 * Triggered by rotorcraft_start.
 * Yields to rotorcraft_collect.
 * Throws rotorcraft_e_sys, rotorcraft_e_connection.
 */
genom_event
mk_calibrate_imu_start(const rotorcraft_ids_calibration_param_s *calib_param,
                       double tstill, uint16_t nposes,
                       const genom_context self)
{
  uint32_t sps;
  int s;

  sps = 1000/rotorcraft_control_period_ms;
  s = mk_calibration_init(tstill * sps, nposes, sps,
                          calib_param->motion_tolerance);
  if (s) {
    errno = s;
    return mk_e_sys_error("calibration", self);
  }

  warnx("calibration started");
  return rotorcraft_collect;
}

/** Codel mk_calibrate_imu_collect of activity calibrate_imu.
 *
 * Triggered by rotorcraft_collect.
 * Yields to rotorcraft_pause_collect, rotorcraft_main.
 * Throws rotorcraft_e_sys, rotorcraft_e_connection.
 */
genom_event
mk_calibrate_imu_collect(const char path[64], double imu_temp,
                         const rotorcraft_imu *imu,
                         const rotorcraft_mag *mag,
                         const genom_context self)
{
  int32_t still;
  int s;

  s = mk_calibration_collect(
    imu_temp, imu->data(self), mag->data(self), &still);
  switch(s) {
    case 0: break;

    case EAGAIN:
      if (still == 0)
        warnx("acquiring next position, stay still");
      else if (still > 0)
        warnx("calibration acquired pose %d", still);
      return rotorcraft_pause_collect;

    default:
      warnx("calibration aborted");
      if (*path) mk_calibration_log(path);
      mk_calibration_fini(NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
      errno = s;
      return mk_e_sys_error("calibration", self);
  }

  warnx("calibration acquired all poses");
  return rotorcraft_main;
}

/** Codel mk_calibrate_imu_main of activity calibrate_imu.
 *
 * Triggered by rotorcraft_main.
 * Yields to rotorcraft_ether.
 * Throws rotorcraft_e_sys, rotorcraft_e_connection.
 */
genom_event
mk_calibrate_imu_main(const char path[64],
                      const rotorcraft_ids_sensor_time_s_rate_s *rate,
                      rotorcraft_ids_imu_calibration_s *imu_calibration,
                      bool *imu_calibration_updated,
                      const genom_context self)
{
  double maxa[3], maxw[3], avga, avgw;
  int s;

  s = mk_calibration_acc(imu_calibration->ascale, imu_calibration->abias);
  if (s) {
    warnx("accelerometer calibration failed");
    goto fail;
  }

  s = mk_calibration_gyr(imu_calibration->gscale, imu_calibration->gbias);
  if (s) {
    warnx("gyroscope calibration failed");
    goto fail;
  }

  if (rate->mag > 0.) {
    s = mk_calibration_mag(imu_calibration->mscale, imu_calibration->mbias);
    if (s) {
      warnx("magnetometer calibration failed");
      goto fail;
    }
  }

  if (*path) mk_calibration_log(path);

  mk_calibration_fini(
    imu_calibration->astddev,
    imu_calibration->gstddev,
    rate->mag > 0. ? imu_calibration->mstddev : NULL,
    maxa, maxw, &imu_calibration->temp, &avga, &avgw);
  warnx("calibration max acceleration: "
        "x %.2fm/s², y %.2fm/s², z %.2fm/s²", maxa[0], maxa[1], maxa[2]);
  warnx("calibration avg acceleration: %gm/s²", avga);
  warnx("calibration max angular velocity: "
        "x %.2f⁰/s, y %.2f⁰/s, z %.2f⁰/s",
        maxw[0] * 180./M_PI, maxw[1] * 180./M_PI, maxw[2] * 180./M_PI);
  warnx("calibration avg angular velocity: %gm/s", avgw);

  *imu_calibration_updated = true;
  return rotorcraft_ether;

fail:
  if (*path) mk_calibration_log(path);
  mk_calibration_fini(NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
  errno = s;
  return mk_e_sys_error("calibration", self);
}


/* --- Activity calibrate_mag ------------------------------------------- */

/** Codel mk_calibrate_mag_start of activity calibrate_mag.
 *
 * Triggered by rotorcraft_start.
 * Yields to rotorcraft_collect.
 * Throws rotorcraft_e_sys, rotorcraft_e_connection.
 */
genom_event
mk_calibrate_mag_start(const rotorcraft_ids_calibration_param_s *calib_param,
                       double tstill, const genom_context self)
{
  return mk_calibrate_imu_start(calib_param, tstill, 2, self);
}

/** Codel mk_calibrate_imu_collect of activity calibrate_mag.
 *
 * Triggered by rotorcraft_collect.
 * Yields to rotorcraft_pause_collect, rotorcraft_main.
 * Throws rotorcraft_e_sys, rotorcraft_e_connection.
 */
/* already defined in service calibrate_imu */


/** Codel mk_calibrate_mag_main of activity calibrate_mag.
 *
 * Triggered by rotorcraft_main.
 * Yields to rotorcraft_ether.
 * Throws rotorcraft_e_sys, rotorcraft_e_connection.
 */
genom_event
mk_calibrate_mag_main(const char path[64],
                      rotorcraft_ids_imu_calibration_s *imu_calibration,
                      bool *imu_calibration_updated,
                      const genom_context self)
{
  int s;

  s = mk_calibration_mag(imu_calibration->mscale, imu_calibration->mbias);
  if (s) {
    warnx("magnetometer calibration failed");
    goto fail;
  }

  if (*path) mk_calibration_log(path);

  mk_calibration_fini(
    NULL, NULL, imu_calibration->mstddev, NULL, NULL, NULL, NULL, NULL);

  *imu_calibration_updated = true;
  return rotorcraft_ether;

fail:
  mk_calibration_fini(NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
  errno = s;
  return mk_e_sys_error("calibration", self);
}


/* --- Activity set_zero ------------------------------------------------ */

/** Codel mk_avgsensors_start of activity set_zero.
 *
 * Triggered by rotorcraft_start.
 * Yields to rotorcraft_collect.
 * Throws rotorcraft_e_sys.
 */
genom_event
mk_avgsensors_start(rotorcraft_accum accum[3],
                    const genom_context self)
{
  (void)self;

  accum[0] = accum[1] = accum[2] = (rotorcraft_accum){
    .data = {0.},
    .count = 0,
    .last = {0}
  };

  return rotorcraft_collect;
}

/** Codel mk_avgsensors_collect of activity set_zero.
 *
 * Triggered by rotorcraft_collect.
 * Yields to rotorcraft_pause_collect, rotorcraft_main.
 * Throws rotorcraft_e_sys.
 */
genom_event
mk_avgsensors_collect(const rotorcraft_imu *imu,
                      const rotorcraft_mag *mag,
                      rotorcraft_accum accum[3], double *duration,
                      const genom_context self)
{
  or_pose_estimator_state *imu_data = imu->data(self);
  or_pose_estimator_state *mag_data = mag->data(self);

  if (imu_data->avel._present &&
      (imu_data->ts.sec != accum[0].last.sec ||
       imu_data->ts.nsec != accum[0].last.sec)) {
    accum[0].data[0] += imu_data->avel._value.wx;
    accum[0].data[1] += imu_data->avel._value.wy;
    accum[0].data[2] += imu_data->avel._value.wz;
    accum[0].count++;
    accum[0].last = imu_data->ts;
  }

  if (imu_data->acc._present &&
      (imu_data->ts.sec != accum[1].last.sec ||
       imu_data->ts.nsec != accum[1].last.sec)) {
    accum[1].data[0] += imu_data->acc._value.ax;
    accum[1].data[1] += imu_data->acc._value.ay;
    accum[1].data[2] += imu_data->acc._value.az;
    accum[1].count++;
    accum[1].last = imu_data->ts;
  }

  if (mag_data->att._present &&
      (mag_data->ts.sec != accum[2].last.sec ||
       mag_data->ts.nsec != accum[2].last.sec)) {
    accum[2].data[0] += mag_data->att._value.qx;
    accum[2].data[1] += mag_data->att._value.qy;
    accum[2].data[2] += mag_data->att._value.qz;
    accum[2].count++;
    accum[2].last = imu_data->ts;
  }

  *duration -= rotorcraft_control_period_ms / 1e3;
  if (*duration > 0.) return rotorcraft_pause_collect;

  if (accum[0].count == 0 && accum[1].count == 0 && accum[2].count == 0) {
    errno = EIO;
    return mk_e_sys_error("set_zero", self);
  }

  return rotorcraft_main;
}

/** Codel mk_set_zero of activity set_zero.
 *
 * Triggered by rotorcraft_main.
 * Yields to rotorcraft_ether.
 * Throws rotorcraft_e_sys.
 */
genom_event
mk_set_zero(rotorcraft_accum accum[3],
            rotorcraft_ids_imu_calibration_s *imu_calibration,
            bool *imu_calibration_updated, const genom_context self)
{
  double roll, pitch;
  double cr, cp, sr, sp;
  double r[9];

  /* gyro bias */
  mk_set_zero_velocity(accum, imu_calibration, imu_calibration_updated, self);

  /* accelerometer rotation */
  if (accum[1].count) {
    roll = atan2(accum[1].data[1], accum[1].data[2]);
    cr = cos(roll);  sr = sin(roll);
    pitch = atan2(-accum[1].data[0], hypot(accum[1].data[1], accum[1].data[2]));
    cp = cos(pitch); sp = sin(pitch);

    r[0] = cp;   r[1] = sr * sp;  r[2] = cr * sp;
    r[3] = 0.;   r[4] = cr;       r[5] = -sr;
    r[6] = -sp;  r[7] = cp * sr;  r[8] = cr * cp;

    mk_calibration_rotate(r, imu_calibration->gscale);
    mk_calibration_rotate(r, imu_calibration->ascale);
    *imu_calibration_updated = true;
  }

  return rotorcraft_ether;
}


/* --- Activity set_zero_velocity --------------------------------------- */

/** Codel mk_avgsensors_start of activity set_zero_velocity.
 *
 * Triggered by rotorcraft_start.
 * Yields to rotorcraft_collect.
 * Throws rotorcraft_e_sys.
 */
/* already defined in service set_zero */


/** Codel mk_avgsensors_collect of activity set_zero_velocity.
 *
 * Triggered by rotorcraft_collect.
 * Yields to rotorcraft_pause_collect, rotorcraft_main.
 * Throws rotorcraft_e_sys.
 */
/* already defined in service set_zero */


/** Codel mk_set_zero_velocity of activity set_zero_velocity.
 *
 * Triggered by rotorcraft_main.
 * Yields to rotorcraft_ether.
 * Throws rotorcraft_e_sys.
 */
genom_event
mk_set_zero_velocity(rotorcraft_accum accum[3],
                     rotorcraft_ids_imu_calibration_s *imu_calibration,
                     bool *imu_calibration_updated,
                     const genom_context self)
{
  (void)self;

  /* gyro bias */
  if (accum[0].count) {
    /* negate the bias to substract offset to the calibration */
    accum[0].data[0] = -accum[0].data[0] / accum[0].count;
    accum[0].data[1] = -accum[0].data[1] / accum[0].count;
    accum[0].data[2] = -accum[0].data[2] / accum[0].count;

    mk_calibration_bias(accum[0].data,
                        imu_calibration->gscale, imu_calibration->gbias);
    *imu_calibration_updated = true;
  }

  return rotorcraft_ether;
}


/* --- Activity start --------------------------------------------------- */

/** Codel mk_start_start of activity start.
 *
 * Triggered by rotorcraft_start.
 * Yields to rotorcraft_pause_start, rotorcraft_monitor.
 * Throws rotorcraft_e_connection, rotorcraft_e_started,
 *        rotorcraft_e_sys, rotorcraft_e_rotor_failure,
 *        rotorcraft_e_rotor_stopped, rotorcraft_e_rate,
 *        rotorcraft_e_rotor_not_disabled.
 */
genom_event
mk_start_start(const rotorcraft_conn_s *conn,
               const rotorcraft_ids_servo_s *servo, uint32_t *timeout,
               uint16_t *state,
               const rotorcraft_ids_rotor_data_s rotor_data[8],
               const genom_context self)
{
  size_t i;
  uint32_t m;

  if (!conn) return rotorcraft_e_connection(self);
  for(i = 0; i < or_rotorcraft_max_rotors; i++) {
    if (rotor_data[i].state.disabled) continue;
    if (rotor_data[i].state.spinning) return rotorcraft_e_started(self);
  }

  *timeout = servo->timeout * 1e3 / rotorcraft_control_period_ms;
  *state = 0;
  for(i = 0; i < or_rotorcraft_max_rotors; i++) {
    if (rotor_data[i].state.disabled) continue;

    for(m = 0; m < conn->n; m++) {
      if (i + 1 < conn->chan[m].minid) continue;
      if (i + 1 > conn->chan[m].maxid) continue;

      mk_send_msg(&conn->chan[m], "g%1", (uint8_t){i+1});
      break;
    }

    /* wait until motor have cleared any emergency flag */
    if (rotor_data[i].state.emerg) return rotorcraft_pause_start;
  }

  return rotorcraft_monitor;
}

/** Codel mk_start_monitor of activity start.
 *
 * Triggered by rotorcraft_monitor.
 * Yields to rotorcraft_pause_monitor, rotorcraft_ether.
 * Throws rotorcraft_e_connection, rotorcraft_e_started,
 *        rotorcraft_e_sys, rotorcraft_e_rotor_failure,
 *        rotorcraft_e_rotor_stopped, rotorcraft_e_rate,
 *        rotorcraft_e_rotor_not_disabled.
 */
genom_event
mk_start_monitor(const rotorcraft_conn_s *conn,
                 const rotorcraft_ids_sensor_time_s *sensor_time,
                 uint32_t *timeout, uint16_t *state,
                 const rotorcraft_ids_rotor_data_s rotor_data[8],
                 const genom_context self)
{
  rotorcraft_e_rotor_stopped_detail s;
  rotorcraft_e_rotor_failure_detail e;
  rotorcraft_e_rotor_not_disabled_detail d;
  rotorcraft_e_rate_detail erate;
  uint32_t m;
  size_t i;
  bool complete;

  (*timeout)--;
  complete = true;
  for(i = 0; i < or_rotorcraft_max_rotors; i++) {
    if (rotor_data[i].state.disabled) {
      if (rotor_data[i].state.starting || rotor_data[i].state.spinning) {
        mk_stop(conn, rotor_data, self);
        d.id = 1 + i;
        return rotorcraft_e_rotor_not_disabled(&d, self);
      }

      continue;
    }

    if (rotor_data[i].state.starting) *state |= 1 << i;
    if (rotor_data[i].state.spinning) continue;

    if (rotor_data[i].state.emerg) {
      mk_stop(conn, rotor_data, self);
      e.id = 1 + i;
      return rotorcraft_e_rotor_failure(&e, self);
    }
    if ((!rotor_data[i].state.starting && (*state & (1 << i)))) {
      mk_stop(conn, rotor_data, self);
      s.id = 1 + i;
      return rotorcraft_e_rotor_stopped(&s, self);
    }

    /* resend startup message every 100 periods */
    if (!rotor_data[i].state.starting && *timeout % 100 == 0)
      for(m = 0; m < conn->n; m++) {
        if (i + 1 < conn->chan[m].minid) continue;
        if (i + 1 > conn->chan[m].maxid) continue;

        mk_send_msg(&conn->chan[m], "g%1", (uint8_t){i+1});
        break;
      }

    complete = false;
  }

  if (!complete) {
    if (!*timeout) {
      mk_stop(conn, rotor_data, self);
      errno = EAGAIN;
      return mk_e_sys_error("start", self);
    }
    return rotorcraft_pause_monitor;
  }

  /* check sensor rate */
#define rate_less80p(dev)                                               \
  (erate = (rotorcraft_e_rate_detail){ #dev },                          \
   (sensor_time->measured_rate.dev < 0.8 * sensor_time->rate.dev))

  if (rate_less80p(imu) || rate_less80p(mag) || rate_less80p(motor)) {
    if (!*timeout) {
      mk_stop(conn, rotor_data, self);
      return rotorcraft_e_rate(&erate, self);
    }
    return rotorcraft_pause_monitor;
  }
#undef rate_less80p

  return rotorcraft_ether;
}

/** Codel mk_start_stop of activity start.
 *
 * Triggered by rotorcraft_stop.
 * Yields to rotorcraft_pause_stop, rotorcraft_ether.
 * Throws rotorcraft_e_connection, rotorcraft_e_started,
 *        rotorcraft_e_sys, rotorcraft_e_rotor_failure,
 *        rotorcraft_e_rotor_stopped, rotorcraft_e_rate,
 *        rotorcraft_e_rotor_not_disabled.
 */
genom_event
mk_start_stop(const rotorcraft_conn_s *conn,
              const rotorcraft_ids_rotor_data_s rotor_data[8],
              const genom_context self)
{
  genom_event e;

  e = mk_stop(conn, rotor_data, self);
  if (e == rotorcraft_ether) return rotorcraft_ether;

  return rotorcraft_pause_stop;
}


/* --- Activity servo --------------------------------------------------- */

/** Codel mk_servo_start of activity servo.
 *
 * Triggered by rotorcraft_start.
 * Yields to rotorcraft_main.
 * Throws rotorcraft_e_connection, rotorcraft_e_rotor_failure,
 *        rotorcraft_e_rotor_stopped, rotorcraft_e_rate,
 *        rotorcraft_e_input.
 */
genom_event
mk_servo_start(double *scale, const genom_context self)
{
  (void)self;

  *scale = 0.;
  return rotorcraft_main;
}

/** Codel mk_servo_main of activity servo.
 *
 * Triggered by rotorcraft_main.
 * Yields to rotorcraft_pause_main, rotorcraft_stop.
 * Throws rotorcraft_e_connection, rotorcraft_e_rotor_failure,
 *        rotorcraft_e_rotor_stopped, rotorcraft_e_rate,
 *        rotorcraft_e_input.
 */
genom_event
mk_servo_main(const rotorcraft_conn_s *conn,
              const rotorcraft_ids_sensor_time_s *sensor_time,
              rotorcraft_ids_rotor_data_s rotor_data[8],
              const rotorcraft_rotor_input *rotor_input,
              const rotorcraft_ids_servo_s *servo, double *scale,
              const genom_context self)
{
  or_rotorcraft_input *input_data;
  rotorcraft_e_rate_detail erate;
  struct timeval tv;
  genom_event e;
  size_t i;

  if (!conn) return rotorcraft_e_connection(self);

  /* update input */
  if (rotor_input->read(self)) return rotorcraft_e_input(self);

  input_data = rotor_input->data(self);
  if (!input_data) return rotorcraft_e_input(self);

  or_rotorcraft_rotor_control desired = input_data->desired;

  /* watchdog on input */
  gettimeofday(&tv, NULL);
  if (tv.tv_sec + 1e-6*tv.tv_usec >
      0.5 + input_data->ts.sec + 1e-9*input_data->ts.nsec) {

    *scale -= 2e-3 * rotorcraft_control_period_ms / servo->ramp;
    if (*scale < 0.) {
      mk_stop(conn, rotor_data, self);
      return rotorcraft_e_input(self);
    }
  }

  /* check sensor rate */
#define rate_less80p(dev)                                               \
  (erate = (rotorcraft_e_rate_detail){ #dev },                          \
   (sensor_time->measured_rate.dev < 0.8 * sensor_time->rate.dev))

  if (rate_less80p(imu) || rate_less80p(mag) || rate_less80p(motor)) {
    if (*scale >= 1.) warnx("low sensor rate, scaling input down");

    *scale -= 2e-3 * rotorcraft_control_period_ms / servo->ramp;
    if (*scale < 0.) {
      warnx("stopped because of low sensor rate");
      mk_stop(conn, rotor_data, self);
      *scale = 0.;
      return rotorcraft_e_rate(&erate, self);
    }
  }
#undef rate_less80p

  /* check rotors status */
  for(i = 0; i < or_rotorcraft_max_rotors; i++) {
    if (rotor_data[i].state.disabled) continue;
    if (rotor_data[i].state.emerg) {
      rotorcraft_e_rotor_failure_detail e;

      mk_stop(conn, rotor_data, self);
      e.id = 1 + i;
      return rotorcraft_e_rotor_failure(&e, self);
    }
    if (!(rotor_data[i].state.starting || rotor_data[i].state.spinning)) {
      rotorcraft_e_rotor_stopped_detail s;

      mk_stop(conn, rotor_data, self);
      s.id = 1 + i;
      return rotorcraft_e_rotor_stopped(&s, self);
    }
  }

  /* linear input scaling for the first servo->ramp seconds or in case of
   * emergency */
  if (*scale < 1.) {
    bool rampup = true;

    for(i = 0; i < desired._length; i++) {
      /* prevent ramping up until all motors are fully started */
      if (!rotor_data[i].state.spinning) rampup = false;
      desired._buffer[i] *= *scale;
    }

    if (rampup) {
      *scale += 1e-3 * rotorcraft_control_period_ms / servo->ramp;
      if (*scale > 1.) *scale = 1.;
    }
  }

  /* send */
  switch(input_data->control) {
    case or_rotorcraft_velocity:
      e = mk_set_velocity(
        conn, rotor_data, &desired, self);
      if (e) return e;
      break;

    case or_rotorcraft_throttle:
      e = mk_set_throttle(
        conn, rotor_data, &desired, self);
      if (e) return e;
      break;
  }

  return rotorcraft_pause_main;
}

/** Codel mk_servo_stop of activity servo.
 *
 * Triggered by rotorcraft_stop.
 * Yields to rotorcraft_ether.
 * Throws rotorcraft_e_connection, rotorcraft_e_rotor_failure,
 *        rotorcraft_e_rotor_stopped, rotorcraft_e_rate,
 *        rotorcraft_e_input.
 */
genom_event
mk_servo_stop(const rotorcraft_conn_s *conn, const genom_context self)
{
  uint16_t p[or_rotorcraft_max_rotors];
  uint32_t i;
  (void)self;

  for(i = 0; i < or_rotorcraft_max_rotors; i++) p[i] = 32767;

  for(i = 0; i < conn->n; i++)
    mk_send_msg(&conn->chan[i],
                "w%@", p, conn->chan[i].maxid - conn->chan[i].minid + 1);

  return rotorcraft_ether;
}


/* --- Activity stop ---------------------------------------------------- */

/** Codel mk_stop of activity stop.
 *
 * Triggered by rotorcraft_start.
 * Yields to rotorcraft_pause_start, rotorcraft_ether.
 */
genom_event
mk_stop(const rotorcraft_conn_s *conn,
        const rotorcraft_ids_rotor_data_s rotor_data[8],
        const genom_context self)
{
  (void)self;
  struct timeval tv;
  uint32_t i;

  /* stop rotors */
  for(i = 0; i < conn->n; i++)
    if (mk_send_msg(&conn->chan[i], "x"))
      warnx("cannot send to %s", conn->chan[i].path);

  gettimeofday(&tv, NULL);
  for(i = 0; i < or_rotorcraft_max_rotors; i++) {
    if (rotor_data[i].state.disabled) continue;

    /* watchdog on motor data */
    if (tv.tv_sec + 1e-6*tv.tv_usec >
        0.5 + rotor_data[i].state.ts.sec +
        1e-9*rotor_data[i].state.ts.nsec) continue;

    if (rotor_data[i].state.spinning) return rotorcraft_pause_start;
  }

  return rotorcraft_ether;
}


/* --- Activity log ----------------------------------------------------- */

/** Codel rc_log_header of activity log.
 *
 * Triggered by rotorcraft_start.
 * Yields to rotorcraft_ether.
 * Throws rotorcraft_e_sys.
 */
genom_event
rc_log_header(const rotorcraft_ids_imu_calibration_s *imu_calibration,
              const rotorcraft_ids_imu_filter_s *imu_filter,
              const rotorcraft_ids_sensor_time_s_rate_s *rate,
              rotorcraft_log_s **log, const genom_context self)
{
  double gfc[3], afc[3], mfc[3];
  int s;


  /* log header with some config info */
  s = dprintf(
    (*log)->fd, "# logged on %s#\n" /* note that ctime(3) has a \n */,
    ctime(&(time_t){ time(NULL) }));
  if (s < 0) goto err;

  if (rc_log_imu_calibration(imu_calibration, log, self)) goto err;

  rc_get_imu_filter(imu_filter, rate, gfc, afc, mfc, self);
  if (rc_log_imu_filter(gfc, afc, mfc, log, self)) goto err;

  if (rc_log_sensor_rate(rate, log, self)) goto err;

  s = dprintf((*log)->fd, rc_log_header_fmt "\n");
  if (s < 0) goto err;


  /* enable async writes */
  (*log)->req.aio_fildes = (*log)->fd;

  return rotorcraft_ether;
err:
  mk_log_stop(log, self);
  return mk_e_sys_error("log", self);
}


/* --- Activity get_sensor_average -------------------------------------- */

/** Codel mk_avgsensors_start of activity get_sensor_average.
 *
 * Triggered by rotorcraft_start.
 * Yields to rotorcraft_collect.
 * Throws rotorcraft_e_sys.
 */
/* already defined in service set_zero */


/** Codel mk_avgsensors_collect of activity get_sensor_average.
 *
 * Triggered by rotorcraft_collect.
 * Yields to rotorcraft_pause_collect, rotorcraft_main.
 * Throws rotorcraft_e_sys.
 */
/* already defined in service set_zero */


/** Codel mk_get_sensor_average of activity get_sensor_average.
 *
 * Triggered by rotorcraft_main.
 * Yields to rotorcraft_ether.
 * Throws rotorcraft_e_sys.
 */
genom_event
mk_get_sensor_average(rotorcraft_accum accum[3], or_t3d_avel *gyr,
                      or_t3d_acc *acc, or_t3d_pos *mag,
                      const genom_context self)
{
  (void)self;

  if (accum[0].count) {
    *gyr = (or_t3d_avel) {
      .wx = accum[0].data[0] /= accum[0].count,
      .wy = accum[0].data[1] /= accum[0].count,
      .wz = accum[0].data[2] /= accum[0].count
    };
  } else
    *gyr = (or_t3d_avel) { nan(""), nan(""), nan("") };

  if (accum[1].count) {
    *acc = (or_t3d_acc) {
      .ax = accum[1].data[0] /= accum[1].count,
      .ay = accum[1].data[1] /= accum[1].count,
      .az = accum[1].data[2] /= accum[1].count
    };
  } else
    *acc = (or_t3d_acc) { nan(""), nan(""), nan("") };

  if (accum[2].count) {
    *mag = (or_t3d_pos) {
      .x = accum[2].data[0] /= accum[2].count,
      .y = accum[2].data[1] /= accum[2].count,
      .z = accum[2].data[2] /= accum[2].count
    };
  } else
    *mag = (or_t3d_pos) { nan(""), nan(""), nan("") };

  return rotorcraft_ether;
}
