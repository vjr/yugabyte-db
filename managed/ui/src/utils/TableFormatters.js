// Copyright (c) YugaByte, Inc.
import React from 'react';
import moment from 'moment';
import { isValidObject } from './ObjectUtils';
import { YBFormattedNumber } from '../components/common/descriptors';
import { YBLoadingCircleIcon } from '../components/common/indicators';

export function timeFormatter(cell) {
  if (!isValidObject(cell)) {
    return '<span>-</span>';
  } else {
    return moment(cell).format('YYYY/MM/DD H:mm [UTC]ZZ');
  }
}

export function backupConfigFormatter(row, configList) {
  if (row.storageConfigUUID) {
    const storageConfig = configList.find((config) => config.configUUID === row.storageConfigUUID);
    if (storageConfig) return storageConfig.configName;
  }
  return 'Config UUID (Missing)';
}

export function percentFormatter(cell, row) {
  return <YBFormattedNumber value={cell / 100} formattedNumberStyle={'percent'} />;
}

export function successStringFormatter(cell, row) {
  switch (row.status) {
    case 'Success':
    case 'Completed':
      return (
        <span className="yb-success-color">
          <i className="fa fa-check" /> Completed
        </span>
      );
    case 'Initializing':
      return (
        <span className="yb-pending-color">
          <YBLoadingCircleIcon size="inline" /> Initializing
        </span>
      );
    case 'InProgress':
      return (
        <span className="yb-pending-color">
          <YBLoadingCircleIcon size="inline" /> InProcess
        </span>
      );
    case 'Running':
      return (
        <span className="yb-pending-color">
          <YBLoadingCircleIcon size="inline" />
          Pending ({percentFormatter(row.percentComplete, row)})
        </span>
      );
    case 'Failure':
    case 'Failed':
      return (
        <span className="yb-fail-color">
          <i className="fa fa-warning" /> Failed
        </span>
      );
    case 'Deleted':
      return (
        <span className="yb-orange">
          <i className="fa fa-warning" /> Deleted
        </span>
      );
    case 'Stopped':
      return (
        <span className="yb-warn-color">
          <i className="fa fa-ban" /> Aborted
        </span>
      );
    default:
      return (
        <span className="yb-fail-color">
          <i className="fa fa-warning" />
          Unknown
        </span>
      );
  }
}

export function alertSeverityFormatter(cell, row) {
  switch (row.severity) {
    case 'SEVERE':
      return (
        <span className="yb-fail-color">
          <i className="fa fa-warning" /> Error
        </span>
      );
    case 'WARNING':
      return (
        <span className="yb-warn-color">
          <i className="fa fa-warning" /> Warning
        </span>
      );
    default:
      return <span>{row.severity}</span>;
  }
}
