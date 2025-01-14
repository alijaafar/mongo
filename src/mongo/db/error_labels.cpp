/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/error_labels.h"

namespace mongo {

bool ErrorLabelBuilder::isTransientTransactionError() const {
    // Note that we only apply the TransientTransactionError label if the "autocommit" field is
    // present in the session options. When present, "autocommit" will always be false, so we
    // don't check its value. There is no point in returning TransientTransactionError label if
    // we have already tried to abort it. An error code for which isTransientTransactionError()
    // is true indicates a transaction failure with no persistent side effects.
    return _code && _sessionOptions.getTxnNumber() && _sessionOptions.getAutocommit() &&
        mongo::isTransientTransactionError(_code.get(), _wcCode != boost::none, _isCommitOrAbort());
}

bool ErrorLabelBuilder::isRetryableWriteError() const {
    // Do not return RetryableWriteError labels to internal clients (e.g. mongos).
    if (_isInternalClient) {
        return false;
    }

    auto isRetryableWrite = [&]() {
        return _sessionOptions.getTxnNumber() && !_sessionOptions.getAutocommit();
    };

    auto isTransactionCommitOrAbort = [&]() {
        return _sessionOptions.getTxnNumber() && _sessionOptions.getAutocommit() &&
            _isCommitOrAbort();
    };

    // Return with RetryableWriteError label on retryable error codes for retryable writes or
    // transactions commit/abort.
    if (isRetryableWrite() || isTransactionCommitOrAbort()) {
        if ((_code && ErrorCodes::isRetriableError(_code.get())) ||
            (_wcCode && ErrorCodes::isRetriableError(_wcCode.get()))) {
            return true;
        }
    }
    return false;
}

bool ErrorLabelBuilder::isNonResumableChangeStreamError() const {
    return _code && ErrorCodes::isNonResumableChangeStreamError(_code.get());
}

void ErrorLabelBuilder::build(BSONArrayBuilder& labels) const {
    // PLEASE CONSULT DRIVERS BEFORE ADDING NEW ERROR LABELS.
    bool hasTransientTransactionError = false;
    if (isTransientTransactionError()) {
        labels << ErrorLabel::kTransientTransaction;
        hasTransientTransactionError = true;
    }
    if (isRetryableWriteError()) {
        // RetryableWriteError and TransientTransactionError are mutually exclusive.
        invariant(!hasTransientTransactionError);
        labels << ErrorLabel::kRetryableWrite;
    }
    if (isNonResumableChangeStreamError()) {
        labels << ErrorLabel::kNonResumableChangeStream;
    }
    return;
}

bool ErrorLabelBuilder::_isCommitOrAbort() const {
    return _commandName == "commitTransaction" || _commandName == "coordinateCommitTransaction" ||
        _commandName == "abortTransaction";
}

BSONObj getErrorLabels(const OperationSessionInfoFromClient& sessionOptions,
                       const std::string& commandName,
                       boost::optional<ErrorCodes::Error> code,
                       boost::optional<ErrorCodes::Error> wcCode,
                       bool isInternalClient) {
    BSONArrayBuilder labelArray;

    ErrorLabelBuilder labelBuilder(sessionOptions, commandName, code, wcCode, isInternalClient);
    labelBuilder.build(labelArray);

    return (labelArray.arrSize() > 0) ? BSON("errorLabels" << labelArray.arr()) : BSONObj();
}

bool isTransientTransactionError(ErrorCodes::Error code,
                                 bool hasWriteConcernError,
                                 bool isCommitOrAbort) {
    bool isTransient;
    switch (code) {
        case ErrorCodes::WriteConflict:
        case ErrorCodes::LockTimeout:
        case ErrorCodes::PreparedTransactionInProgress:
            isTransient = true;
            break;
        default:
            isTransient = false;
            break;
    }

    isTransient |= ErrorCodes::isSnapshotError(code) || ErrorCodes::isNeedRetargettingError(code) ||
        code == ErrorCodes::StaleDbVersion;

    if (isCommitOrAbort) {
        // On NoSuchTransaction it's safe to retry the whole transaction only if the data cannot be
        // rolled back.
        isTransient |= code == ErrorCodes::NoSuchTransaction && !hasWriteConcernError;
    } else {
        isTransient |= ErrorCodes::isRetriableError(code) || code == ErrorCodes::NoSuchTransaction;
    }

    return isTransient;
}

}  // namespace mongo
