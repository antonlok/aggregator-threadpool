/**
 * File: news-aggregator.cc
 * --------------------------------
 * Presents the implementation of the NewsAggregator class.
 */

#include "news-aggregator.h"

#include <getopt.h>
#include <libxml/catalog.h>
#include <libxml/parser.h>

#include <algorithm>
#include <iomanip>
#include <iostream>
#include <memory>
#include <thread>
#include <utility>

#include "html-document-exception.h"
#include "html-document.h"
#include "ostreamlock.h"
#include "rss-feed-exception.h"
#include "rss-feed-list-exception.h"
#include "rss-feed-list.h"
#include "rss-feed.h"
#include "semaphore.h"
#include "string-utils.h"
#include "utils.h"
using namespace std;


static const string kDefaultRSSFeedListURL = "small-feed.xml";
NewsAggregator* NewsAggregator::createNewsAggregator(int argc, char* argv[]) {
  struct option options[] = {
      {"verbose", no_argument, NULL, 'v'},
      {"quiet", no_argument, NULL, 'q'},
      {"url", required_argument, NULL, 'u'},
      {NULL, 0, NULL, 0},
  };

  string rssFeedListURI = kDefaultRSSFeedListURL;
  bool verbose = true;
  while (true) {
    int ch = getopt_long(argc, argv, "vqu:", options, NULL);
    if (ch == -1) break;
    switch (ch) {
      case 'v':
        verbose = true;
        break;
      case 'q':
        verbose = false;
        break;
      case 'u':
        rssFeedListURI = optarg;
        break;
      default:
        NewsAggregatorLog::printUsage("Unrecognized flag.", argv[0]);
    }
  }

  argc -= optind;
  if (argc > 0) NewsAggregatorLog::printUsage("Too many arguments.", argv[0]);
  return new NewsAggregator(rssFeedListURI, verbose);
}

void NewsAggregator::buildIndex() {
  if (built) return;
  built = true;  // optimistically assume it'll all work out
  xmlInitParser();
  xmlInitializeCatalog();
  processAllFeeds();
  xmlCatalogCleanup();
  xmlCleanupParser();
}

void NewsAggregator::queryIndex() const {
  static const size_t kMaxMatchesToShow = 15;
  while (true) {
    cout << "Enter a search term [or just hit <enter> to quit]: ";
    string response;
    getline(cin, response);
    response = trim(response);
    if (response.empty()) break;
    const vector<pair<Article, int>>& matches = index.getMatchingArticles(response);
    if (matches.empty()) {
      cout << "Ah, we didn't find the term \"" << response << "\". Try again." << endl;
    } else {
      cout << "That term appears in " << matches.size() << " article"
           << (matches.size() == 1 ? "" : "s") << ".  ";
      if (matches.size() > kMaxMatchesToShow)
        cout << "Here are the top " << kMaxMatchesToShow << " of them:" << endl;
      else if (matches.size() > 1)
        cout << "Here they are:" << endl;
      else
        cout << "Here it is:" << endl;
      size_t count = 0;
      for (const pair<Article, int>& match : matches) {
        if (count == kMaxMatchesToShow) break;
        count++;
        string title = match.first.title;
        if (shouldTruncate(title)) title = truncate(title);
        string url = match.first.url;
        if (shouldTruncate(url)) url = truncate(url);
        string times = match.second == 1 ? "time" : "times";
        cout << "  " << setw(2) << setfill(' ') << count << ".) "
             << "\"" << title << "\" [appears " << match.second << " " << times << "]." << endl;
        cout << "       \"" << url << "\"" << endl;
      }
    }
  }
}

static const size_t kNumFeedWorkers = 10;
static const size_t kNumArticleWorkers = 50;
NewsAggregator::NewsAggregator(const string& rssFeedListURI, bool verbose) : log(verbose), rssFeedListURI(rssFeedListURI), built(false), feedPool(kNumFeedWorkers), articlePool(kNumArticleWorkers) {}

void NewsAggregator::processAllFeeds() {
  RSSFeedList feedList(rssFeedListURI);
  try {
    feedList.parse();
  } 
  catch (const RSSFeedListException& rfle) {
    return;
  }
  
  const map<string, string>& feeds = feedList.getFeeds();
  if (feeds.empty()) {
    cout << "Feed list is technically well-formed, but it's empty!" << endl;
    return;
  }
  
  launchFeedPool(feeds);

  for (const pair<const pair<string, string>, pair<Article, vector<string>>>& articleBundle : intermediateIndex) {
    index.add(articleBundle.second.first, articleBundle.second.second);
  }
}

void NewsAggregator::launchFeedPool(const map<string, string>& feeds) {
  for (const pair<const string, string>& currentFeed : feeds) {
    feedPool.schedule([this, currentFeed] {
      string feedURL = currentFeed.first;
      string feedTitle = currentFeed.second;

      seenURLsLock.lock();
      if (seenURLs.count(feedURL)) {
        seenURLsLock.unlock();
        return;
      }
      seenURLs.insert(feedURL);
      seenURLsLock.unlock();

      RSSFeed feed(feedURL);
      try {
        feed.parse();
      } 
      catch (const RSSFeedException& rfe) {
        return;
      }

      const vector<Article>& articles = feed.getArticles();

      if (articles.empty()) {
        cout << "Feed is technically well-formed, but it's empty!" << endl;
        return;
      }
      launchArticlePool(articles);
    });
  }
  feedPool.wait();
}

void NewsAggregator::launchArticlePool(const vector<Article>& articles) {
  for (Article currentArticle : articles) {
    articlePool.schedule([this, currentArticle] {
      string articleURL = currentArticle.url;
      
      seenURLsLock.lock();
      if (seenURLs.count(articleURL)) {
        seenURLsLock.unlock();
        return;
      }
      seenURLs.insert(articleURL);
      seenURLsLock.unlock();

      string articleTitle = currentArticle.title;
      pair<string, string> articleIden = make_pair(articleTitle, getURLServer(articleURL));

      HTMLDocument document(articleURL);
      try {
        document.parse();
      } 
      catch (const HTMLDocumentException& hde) {
        return;
      }

      const vector<string>& origTokens = document.getTokens();
      vector<string> sortedTokens = origTokens;
      sort(sortedTokens.begin(), sortedTokens.end());

      intermediateIndexLock.lock();
      if (intermediateIndex.count(articleIden)) {
        string existingURL = intermediateIndex[articleIden].first.url;
        Article revisedArticle = currentArticle;
        revisedArticle.url = existingURL < articleURL ? existingURL : articleURL;
        vector<string> existingTokens = intermediateIndex[articleIden].second;
        vector<string> intersectTokens;
        set_intersection(intermediateIndex[articleIden].second.cbegin(), intermediateIndex[articleIden].second.cend(), sortedTokens.cbegin(), sortedTokens.cend(), back_inserter(intersectTokens));
        intermediateIndex[articleIden] = make_pair(revisedArticle, intersectTokens);
        intermediateIndexLock.unlock();
      } 
      else {
        intermediateIndex[articleIden] = make_pair(currentArticle, sortedTokens);
        intermediateIndexLock.unlock();
      }
    });
  }
  articlePool.wait();
}