#pragma once

#include <vector>

#include "stream.hh"
#include "loc.hh"

class port_interf_t;
class soft_config_t;

namespace dsa {
namespace sim {

/*!
 * \brief The base class of determining which streams to be executed.
 */
struct StreamArbiter {
  /*!
   * \brief Determine the streams to be executed according to the state of each port.
   */
  virtual std::vector<base_stream_t*> Arbit(soft_config_t &sc, port_interf_t &pi) = 0;
};

struct RoundRobin : StreamArbiter {
  /*!
   * \brief The last executed streams of round robin.
   */
  std::vector<std::vector<int>> last_executed;

  RoundRobin() : last_executed(2, std::vector<int>(LOC::TOTAL, 0)) {}

  std::vector<base_stream_t*> Arbit(soft_config_t &sc, port_interf_t &pi) override;
};

}
}