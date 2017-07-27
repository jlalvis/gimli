#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
Created on Thu Dec 04 14:09:36 2014

@author: Marcus
"""
import sys
import numpy as np


class ReadAHL(object):
    """Class reading seismic refraction format provided by Uppsala University.

    Supply a filename and a delimiter character. The delimiter is used to
    find the header string which in turn is used to label and extract the
    columns.
    """

    def __init__(self, filename, maxsensorid, delimiter='|'):
        self.header = None
        self.delimiter = delimiter
        self.filename = filename
        self.alldata = []  # contains all of the data (unaltered)
        self.skiprows = 0  # how many leading rows are in the file
        self.labels = dict()  # labels and corresponding column
        self.maxsensorid = maxsensorid  # for excluding geophones ( >= )
        self.sensorcols = []  # the columns that contain info about the sensors
        self.shotcols = []  # the columns that contain info about the shot
        self.positionlist = []  # the list of sensor and shot positions
        self.use_xz_only = True

    def __call__(self):
        self.load()
        self.convert()

    def __str__(self):
        s = 'header line: ' + self.header
        s += '\nalldata length: {}\nfirst row: '.format(len(self.alldata))
        s += str(self.alldata[0, :]) + '\nlast row: '
        s += str(self.alldata[-1, :])

        return s

    def __repr__(self):
        in_arg = "'" + self.filename + "'" + \
            ', maxsensorid=' + str(self.maxsensorid) + \
            ', delimiter=' + "'" + self.delimiter + "'"
        return self.__class__.__name__ + "(" + in_arg + ")"

    def _extractlabels(self):
        """Extract the labels from the header line."""

        labels = self.header.split(self.delimiter)
        n = 0
        for l in labels:
            l = l.strip()
            if len(l) > 0:
                self.labels[l] = n  # add labels as keys and colnums as value
                if l.find('DELAY') >= 0 or l.find('REC') >= 0:
                    self.sensorcols.append(n)
                else:
                    self.shotcols.append(n)
                n += 1

    def _extractheader(self):
        """Search for the header and extract it."""
        # open the file and get some info out:
        # number of lines to skip and the header
        with open(self.filename, 'r') as f:
            # loop until we find the delimiter as the first character of line
            firstchar = ''
            while firstchar is not self.delimiter:
                self.header = f.readline().strip()
                firstchar = self.header[0]
                self.skiprows += 1  # update number of leading rows in the file

        self._extractlabels()  # split the header line into individual labels

    def load(self):
        """Main method for actually processing/reading the data in file."""
        # this will determine number of leading rows and extract labels etc...
        self._extractheader()

        with open(self.filename, 'r') as f:
            # skip some rows as determined by _extractheader()
            for _ in range(self.skiprows):
                f.next()

            # build a 2D list where each row is a line from the file
            currentshot = 0
            for line in f:
                # convert to int so we can use
                datarow_str = line.strip().split()
                datarow = [int(d) for d in datarow_str if len(d) > 0]

                # normal case is that we have data without shotid, so add it
                if len(datarow) < len(self.labels):
                    datarow.insert(0, currentshot)
                else:
                    currentshot = datarow[0]

                self.alldata.append(datarow)

        # convert it all to numpy array for easier manipulation later
        self.alldata = np.asarray(self.alldata)

    def convert(self):
        """Extract sensors and build a list for BERT/GIMLi indexing.

        Also determine relevant column numbers for the data.
        """

        if len(self.alldata) is 0:
            raise ValueError('Data not read yet!')

        shots_col = self.labels['SHOT_PEG']
        sensors_col = self.labels['REC_PEG']

        # cut out the unwanted sensors
        data = self.alldata[self.alldata[:, shots_col] <= self.maxsensorid, :]
        # ...and shotpositions
        data = data[data[:, sensors_col] <= self.maxsensorid, :]

        shots_uniq = np.unique(data[:, shots_col])
        sensors_uniq = np.unique(data[:, sensors_col])
        all_uniq = np.unique(np.row_stack((shots_uniq[:, np.newaxis],
                                           sensors_uniq[:, np.newaxis])))

        # remap the sensor ids to indices starting from 1 going to N
        sensor_map = zip(all_uniq, np.arange(1, len(all_uniq)+1, dtype=int))
        for old, new in sensor_map:
            data[data[:, sensors_col] == old, sensors_col] = new
            data[data[:, shots_col] == old, shots_col] = new

        # now create a list of unique (x, y, z)'s
        final_uniq_list, flat_idx = np.unique(
            data[:, [shots_col, sensors_col]], return_index=True)
        idx = np.unravel_index(flat_idx,
                               data[:, [shots_col, sensors_col]].shape)
        # rows and corresponding columns (column==0 means sensor, 1 means shot)
        # Here we build a list of which columns to pick the positions from
        # TODO: Not sure that -3 is always correct!
        column_list = [np.asarray(self.sensorcols[-3:]) if c == 1
                       else np.asarray(self.shotcols[-3:]) for c in idx[1]]

        unique_xyz = data[idx[0][:, np.newaxis], column_list]

#        mpl.plot(unique_xyz[:, 1], unique_xyz[:, 2], 'b.-')
#        mpl.show(block=False)

        if self.use_xz_only:
            x = unique_xyz[:, 1]
            y = unique_xyz[:, 2]
            xx = np.cumsum(np.sqrt(np.diff(x)**2 + np.diff(y)**2))
            xx = xx.tolist()
            xx.insert(0, 0.0)
            xx = np.asarray(xx)
            final_pos = np.column_stack((xx[:, np.newaxis],
                                         unique_xyz[:, 0])) / 10.0
        else:
            # TODO: Not sure that [1,2,0] is always the correct order!
            final_pos = unique_xyz[:, [1, 2, 0]] / 10.0

        # change the order of the positions to be in x,y,z order
        # also change to [m] instead of [dm]
        self.save(data=final_pos, desc='pos')

        sgt_cols = [self.labels['SHOT_PEG'], self.labels['REC_PEG'],
                    self.labels['X1:DELAY']]
        sgt = data[:, sgt_cols]
#        valid = sgt[:, -1] > 0
        sgt = sgt.astype(float)
        sgt[:, -1] /= 1000.0  # milliseconds to seconds

#        self.save(data=np.column_stack((sgt, valid)), desc='sgt')
        self.save(data=sgt, desc='sgt')

    def save(self, data, desc):
        """Write the converted data to disk."""
        fname_in = self.filename
        fname_out = fname_in[:fname_in.rfind('.')] + '.sgt'

        if desc == 'pos':
            mode = 'w'
        else:
            mode = 'a'

        with open(fname_out, mode) as f:
            if desc == 'pos':
                if self.use_xz_only:
                    f.write(str(data.shape[0]) + ' # No positions\n#x z\n')
                else:
                    f.write(str(data.shape[0]) + ' # No positions\n#x y z\n')

                np.savetxt(f, data, fmt='%.2f')
            elif desc == 'sgt':
                f.write(str(data.shape[0]) + ' # Number of data\n#s g t\n')
                np.savetxt(f, data, fmt='%i %i %.4f')
            else:
                raise ValueError('Invalid description of\
                data to be written: {}'.format(desc))

if __name__ == '__main__':
    print('AHL-file reader')
    if len(sys.arv) > 1:
        datafile = sys.argv[1]
        ahl = ReadAHL(datafile, maxsensorid=2999, delimiter='|')
