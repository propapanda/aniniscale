/*
* Copyright (C) 2017 by Dmitry Odintsov
* This code is licensed under the MIT license (MIT)
* (http://opensource.org/licenses/MIT)
*/

/*
 *  Usage: aniniscale <x_factor> <y_factor> <in_image> <out_image>
 */

#include <vips/vips8>

#include <cstdlib>
#include <ctime>
#include <future>
#include <iomanip>
#include <iostream>
#include <list>
#include <map>
#include <thread>

using namespace vips;

static int x_factor = 1;
static int y_factor = 1;
static int bandCount = 3;
static int taskPixels = 0;
static int totalPixels = 0;

static const std::time_t startTime = std::time(0);
static volatile std::time_t lastReport = std::time(0);
static volatile std::time_t etaReport = std::time(0);

//! Prints out elapsed time at most once per 5 seconds
void ReportElapsedTime()
{
    if (std::time(0) - lastReport < 5)
    {
        return;
    }

    lastReport = std::time(0);
    std::time_t elapsed = lastReport - startTime;

    char buffer[9];
    if (std::strftime(buffer, sizeof(buffer), "%H:%M:%S", std::gmtime(&elapsed)))
    {
        std::cout << "/" << std::setfill('-') << std::setw(25) << "\\" << std::endl;
        std::cout << "| Time elapsed: " << buffer << " |" << std::endl;
        std::cout << "\\" << std::setfill('-') << std::setw(25) << "/" << std::endl;
    }
    else
    {
        std::cout << "Failed to convert data ¯\\_(ツ)_/¯" << std::endl;
    }
}

//! Prints out ETA at most once per 5 seconds
void EstimateTimeLeft(unsigned int pixelsLeft)
{
    if (std::time(0) - etaReport < 5)
    {
        return;
    }

    etaReport = std::time(0);
    std::time_t elapsed = etaReport - startTime;

    double perSecond = ((double)(totalPixels - pixelsLeft)) / ((double)elapsed);

    std::time_t eta = static_cast<unsigned int>(((double)pixelsLeft) / perSecond);

    char buffer[9];
    if (std::strftime(buffer, sizeof(buffer), "%H:%M:%S", std::gmtime(&eta)))
    {
        std::cout << "/" << std::setfill('-') << std::setw(16) << "\\" << std::endl;
        std::cout << "| ETA: " << buffer << " |" << std::endl;
        std::cout << "\\" << std::setfill('-') << std::setw(16) << "/" << std::endl;
    }
    else
    {
        std::cout << "Failed to convert data ¯\\_(ツ)_/¯" << std::endl;
    }
}

//! Described pool of thread workers
class WorkerPool
{
public:
    typedef std::function<void()> Task;

    //! Worker's routine
    void Worker()
    {
        std::unique_lock<std::mutex> lock(m_mutex);

        std::cout << "Worker ready " << std::this_thread::get_id() << std::endl;

        while (!m_tasks.empty())
        {
            unsigned int pixelsLeft = m_tasks.size() * taskPixels;
            std::cout << "Pixels left: " << pixelsLeft << std::endl;

            //! Before you start working - report current time status
            ReportElapsedTime();
            EstimateTimeLeft(pixelsLeft);

            //! Take a task
            Task task = m_tasks.front();
            m_tasks.pop_front();

            //! Let other workers take tasks as well
            lock.unlock();

            //! Complete your task
            task();

            //! Rinse and repeat
            lock.lock();
        }

        std::cout << "Worker done " << std::this_thread::get_id() << std::endl;
    }

    //! Pushes task to queue (not thread safe, shall be done before workers are initialized)
    void PushTask(Task task)
    {
        m_tasks.push_back(task);
    }

private:
    std::mutex m_mutex;

    std::list<Task> m_tasks;
};

//! Task at hand - process portion of an image
void work(VImage img, std::vector<unsigned char>& out)
{
    //! Get image dimensions
    const unsigned int width = img.width();
    const unsigned int height = img.height();

    //! Calculate tile count (== pixels in the end result)
    const unsigned int x_tiles = width / x_factor;
    const unsigned int y_tiles = height / y_factor;

    //! Calculate size of each tile
    const unsigned int size = x_factor * y_factor;

    //! Calculate color threshold - if color has this much, it is dominating
    const unsigned int win = size / 2;

    //! Iterate over all tiles
    for (unsigned int x = 0; x < x_tiles; ++x)
    {
        for (unsigned int y = 0; y < y_tiles; ++y)
        {
            //! Get original area [x_factor by y_factor]
            VImage area = img.extract_area(x * x_factor, y * y_factor, x_factor, y_factor);

            //! Get pixel data
            const unsigned char* pixels = reinterpret_cast<const unsigned char*>(area.data());

            //! Find dominant color
            // @TODO: there must be some better way to do this. Histograms?
            std::map<unsigned int, unsigned int> colors;
            unsigned int dominant = 0;
            unsigned int domCount = 0;

            //! Iterate over all pixels in original area
            for (unsigned int i = 0; i < size; ++i)
            {
                unsigned int color = 0;

                //! Get color value
                for (int b = 0; b < bandCount; ++b)
                {
                    color |= pixels[i * bandCount + b] << ((bandCount - 1 - b) * 8);
                }

                //! Increase the number of votes for that color and check if it's dominating
                colors[color] += 1;

                if (domCount < colors[color])
                {
                    domCount = colors[color];
                    dominant = color;

                    if (domCount >= win)
                    {
                        break;
                    }
                }
            }

            //! Paint the resulting pixel with dominant color
            for (int b = 0; b < bandCount; ++b)
            {
                out[(y * x_tiles + x) * bandCount + b] = (dominant >> ((bandCount - 1 - b) * 8)) & 0xFF;
            }
        }
    }
}

int main( int argc, char **argv )
{
    //! Initialize VIPS library
    if( VIPS_INIT( argv[0] ) )
    {
        return -1;
    }

    //! Check if we have enough arguments
    // @TODO: Argument correctness check? If arguments are messed up it may behave unexpectedly
    if (argc < 5)
    {
        std::cout << "Not enough arguments" << std::endl;
        std::cout << "Usage: aniniscale <x_factor> <y_factor> <in_image> <out_image>" << std::endl;
        return -1;
    }

    //! Get factors
    x_factor = atoi(argv[1]);
    y_factor = atoi(argv[2]);

    //! If any factor is < 1 we're not going to do a thing
    if (x_factor < 1 || y_factor < 1)
    {
        return -1;
    }

    //! Open the image and check channel count
    VImage img = VImage::new_from_file( argv[3] );
    bandCount = img.bands();

    //! If both factors are 1, we can just save the image
    if (x_factor == 1 && y_factor == 1)
    {
        img.pngsave(argv[4]);
        return 0;
    }

    //! Get image information and estimate how it will be divided
    const unsigned int width = img.width();
    const unsigned int height = img.height();
    const unsigned int x_tiles = width / x_factor;
    const unsigned int y_tiles = height / y_factor;

    //! Arbitrary limit of task's area
    const unsigned int max_x_sections = x_factor * x_factor;
    const unsigned int max_y_sections = y_factor * y_factor;

    totalPixels = width * height;

    //! Check how many threads we can run
    unsigned int workerCount = std::thread::hardware_concurrency();

    //! Make sure it's a multiple of 2
    if (workerCount % 2 != 0)
    {
        workerCount += workerCount % 2;
    }

    //! If we have too much workers, cut their number until we have enough work
    while (workerCount > x_tiles || workerCount > y_tiles)
    {
        workerCount -= 2;
    }

    //! If we ended up without workers, bring back one
    if (0 == workerCount)
    {
        workerCount = 1;
    }

    //! Tasks shall not be too big, so we keep them manageable
    unsigned int x_section = x_tiles / workerCount;
    unsigned int y_section = y_tiles / workerCount;

    while (x_section > max_x_sections)
    {
        x_section /= 2;
    }

    while (y_section > max_y_sections)
    {
        y_section /= 2;
    }

    //! Calculate sizes of each section and their total count
    const unsigned int x_sectionSize = x_section * x_factor;
    const unsigned int y_sectionSize = y_section * y_factor;

    const unsigned int x_sectionCount = width / x_sectionSize;
    const unsigned int y_sectionCount = height / y_sectionSize;

    //! Store total number of pixels on the image for the reporting
    taskPixels = x_sectionSize * y_sectionSize;

    WorkerPool pool;

    std::cout << "Creating tasks" << std::endl;

    std::map<std::pair<unsigned int, unsigned int>, std::vector<unsigned char>> result;

    //! Allocate all needed memory
    for (unsigned int x_task = 0; x_task < x_sectionCount; ++x_task)
    {
        for (unsigned int y_task = 0; y_task < y_sectionCount; ++y_task)
        {
            result[std::pair<unsigned int, unsigned int>(x_task, y_task)].resize(x_section * y_section * bandCount);
        }
    }

    //! Create tasks to process each section
    for (unsigned int x_task = 0; x_task < x_sectionCount; ++x_task)
    {
        for (unsigned int y_task = 0; y_task < y_sectionCount; ++y_task)
        {
            std::pair<unsigned int, unsigned int> coords(x_task, y_task);

            VImage area = img.extract_area(coords.first * x_sectionSize,
                coords.second * y_sectionSize,
                x_sectionSize,
                y_sectionSize);

            pool.PushTask([=, &result](){ work(area, result[std::pair<unsigned int, unsigned int>(x_task, y_task)]); });
        }
    }

    std::cout << "Created " << result.size() << " tasks" << std::endl;

    std::cout << "Total pixels to be processed: " << totalPixels << std::endl;

    //! Spawn workers
    std::vector<std::thread> workers;
    workers.resize(workerCount);
    for (unsigned int i = 0; i < workerCount; ++i)
    {
        workers[i] = std::thread(&WorkerPool::Worker, &pool);
    }

    //! Wait for all workers to finish
    for (unsigned int i = 0; i < workerCount; ++i)
    {
        workers[i].join();
    }

    workers.clear();

    //! Prepare the buffer to store final output
    std::vector<unsigned char> outBuffer;
    outBuffer.resize(x_tiles * y_tiles * bandCount);

    VImage out = VImage::VImage::new_from_memory(outBuffer.data(), outBuffer.size(),
        x_tiles, y_tiles, bandCount, img.format());

    //! Go through worker results and place them in resulting image
    for (auto& r : result)
    {
        const std::pair<unsigned int, unsigned int>& coords = r.first;
        std::vector<unsigned char>& buffer = r.second;
        VImage block = VImage::new_from_memory(buffer.data(), buffer.size(),
            x_section, y_section, bandCount, img.format());

        out = out.insert(block, coords.first * x_section, coords.second * y_section);
    }

    //! Save the image
    out.pngsave(argv[4]);

    //! Deinitialize
    vips_shutdown();

    //! Report elapsed time
    ReportElapsedTime();

    return 0;
}
