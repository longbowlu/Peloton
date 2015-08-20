/*-------------------------------------------------------------------------
 *
 * stdoutfrontendlogger.h
 * file description
 *
 * Copyright(c) 2015, CMU
 *
 * /peloton/src/backend/logging/stdoutfrontendlogger.h
 *
 *-------------------------------------------------------------------------
 */

#pragma once

#include "backend/logging/logrecord.h"
#include "backend/logging/frontendlogger.h"

namespace peloton {
namespace logging {

static std::vector<LogRecord*> stdout_global_queue;

//===--------------------------------------------------------------------===//
// Stdout Frontend Logger 
//===--------------------------------------------------------------------===//

class StdoutFrontendLogger : public FrontendLogger{

  public:

    StdoutFrontendLogger(){ logging_type = LOGGING_TYPE_STDOUT;}

    void MainLoop(void);

    void CollectLogRecord(bool coerce = false);

    void Flush(void);

    // FIXME :: Dummy function
    void Recovery(void) {};

  private:

    size_t GetLogRecordCount(void) const;

    // Hard coded global_queue size for testing
    oid_t stdout_global_queue_size = 1;

};

}  // namespace logging
}  // namespace peloton
