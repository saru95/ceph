// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include <boost/bind.hpp>

#include "common/debug.h"
#include "common/errno.h"
#include "include/stringify.h"
#include "Replayer.h"

#define dout_subsys ceph_subsys_rbd_mirror
#undef dout_prefix
#define dout_prefix *_dout << "rbd-mirror: "

using std::chrono::seconds;
using std::map;
using std::string;
using std::unique_ptr;
using std::vector;

namespace rbd {
namespace mirror {

Replayer::Replayer(RadosRef local_cluster, const peer_t &peer) :
  m_lock(stringify("rbd::mirror::Replayer ") + stringify(peer)),
  m_peer(peer),
  m_local(local_cluster),
  m_remote(new librados::Rados),
  m_replayer_thread(this)
{
}

Replayer::~Replayer()
{
  m_stopping.set(1);
  {
    Mutex::Locker l(m_lock);
    m_cond.Signal();
  }
  m_replayer_thread.join();
}

int Replayer::init()
{
  dout(20) << __func__ << "Replaying for " << m_peer << dendl;

  int r = m_remote->init2(m_peer.client_name.c_str(),
			  m_peer.cluster_name.c_str(), 0);
  if (r < 0) {
    derr << "error initializing remote cluster handle for " << m_peer
	 << " : " << cpp_strerror(r) << dendl;
    return r;
  }

  r = m_remote->conf_read_file(nullptr);
  if (r < 0) {
    derr << "could not read ceph conf for " << m_peer
	 << " : " << cpp_strerror(r) << dendl;
    return r;
  }

  r = m_remote->connect();
  if (r < 0) {
    derr << "error connecting to remote cluster " << m_peer
	 << " : " << cpp_strerror(r) << dendl;
    return r;
  }

  string cluster_uuid;
  r = m_remote->cluster_fsid(&cluster_uuid);
  if (r < 0) {
    derr << "error reading cluster uuid from remote cluster " << m_peer
	 << " : " << cpp_strerror(r) << dendl;
    return r;
  }

  if (cluster_uuid != m_peer.cluster_uuid) {
    derr << "configured cluster uuid does not match actual cluster uuid. "
	 << "expected: " << m_peer.cluster_uuid
	 << " observed: " << cluster_uuid << dendl;
    return -EINVAL;
  }

  dout(20) << __func__ << "connected to " << m_peer << dendl;

  // TODO: make interval configurable
  m_pool_watcher.reset(new PoolWatcher(m_remote, 30, m_lock, m_cond));
  m_pool_watcher->refresh_images();

  return 0;
}

void Replayer::run()
{
  while (!m_stopping.read()) {
    set_sources(m_pool_watcher->get_images());
    m_cond.WaitInterval(g_ceph_context, m_lock, seconds(30));
  }
}

void Replayer::set_sources(const map<int64_t, set<string> > &images)
{
  std::map<int64_t, std::map<std::string,
                 std::unique_ptr<ImageReplayer> > > images_copy;
  m_lock.Lock();
  for( const auto &iter : m_images){
      for(const auto &entry : ((iter).second)){
          images_copy[iter.first].insert(make_pair((entry).first,
                    unique_ptr<ImageReplayer>(new ImageReplayer (*((entry).second)) )));
      }
  }
  m_lock.Unlock();
  // TODO: make stopping and starting ImageReplayers async
  for (auto it = images_copy.begin(); it != images_copy.end();) {
    int64_t pool_id = it->first;
    auto &pool_images = it->second;
    if (images.find(pool_id) == images.end()) {
      images_copy.erase(it++);
      continue;
    }
    for (auto images_it = pool_images.begin();
	 images_it != pool_images.end();) {
      if (images.at(pool_id).find(images_it->first) ==
	  images.at(pool_id).end()) {
	pool_images.erase(images_it++);
      } else {
	++images_it;
      }
    }
    ++it;
  }

  for (const auto &kv : images) {
    int64_t pool_id = kv.first;
    // create entry for pool if it doesn't exist
    auto &pool_replayers = images_copy[pool_id];
    for (const auto &image_id : kv.second) {
      if (pool_replayers.find(image_id) == pool_replayers.end()) {
	unique_ptr<ImageReplayer> image_replayer(new ImageReplayer(m_local,
								   m_remote,
								   pool_id,
								   image_id));
	int r = image_replayer->start();
	if (r < 0) {
	  continue;
	}
	pool_replayers.insert(std::make_pair(image_id, std::move(image_replayer)));
      }
    }
  }
  m_lock.Lock();
  for( const auto &iter : images_copy){
      for(const auto &entry : ((iter).second)){
          m_images[iter.first].insert(make_pair((entry).first,
                    unique_ptr<ImageReplayer>(new ImageReplayer (*((entry).second)) )));
      }
  }
  m_lock.Unlock();
}

} // namespace mirror
} // namespace rbd
